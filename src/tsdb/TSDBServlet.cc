/**
 * This file is part of the "libfnord" project
 *   Copyright (c) 2015 Paul Asmuth
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include "fnord/util/binarymessagewriter.h"
#include "tsdb/TSDBServlet.h"
#include "tsdb/RecordEnvelope.pb.h"
#include "fnord/json/json.h"
#include <fnord/wallclock.h>
#include <fnord/thread/wakeup.h>
#include "fnord/protobuf/MessageEncoder.h"
#include "fnord/protobuf/MessagePrinter.h"
#include "fnord/protobuf/msg.h"
#include <fnord/util/Base64.h>
#include <fnord/fnv.h>
#include <sstable/sstablereader.h>

using namespace fnord;

namespace tsdb {

TSDBServlet::TSDBServlet(TSDBNode* node) : node_(node) {}

void TSDBServlet::handleHTTPRequest(
    RefPtr<http::HTTPRequestStream> req_stream,
    RefPtr<http::HTTPResponseStream> res_stream) {
  req_stream->readBody();
  const auto& req = req_stream->request();
  URI uri(req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);
  res.addHeader("Access-Control-Allow-Origin", "*");

  try {
    if (uri.path() == "/tsdb/insert") {
      insertRecords(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/stream") {
      streamPartition(&req, &res, res_stream, &uri);
      return;
    }

    if (uri.path() == "/tsdb/partition_info") {
      fetchPartitionInfo(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    res.setStatus(fnord::http::kStatusNotFound);
    res.addBody("not found");
    res_stream->writeResponse(res);
  } catch (const Exception& e) {
    res.setStatus(http::kStatusInternalServerError);
    res.addBody(StringUtil::format("error: $0: $1", e.getTypeName(), e.getMessage()));
    res_stream->writeResponse(res);
  }
}

void TSDBServlet::insertRecords(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto record_list = msg::decode<RecordEnvelopeList>(req->body());

  // FIXPAUL this is slow, we should group records by partition and then do a
  // batch insert per partition
  for (const auto& record : record_list.records()) {
    auto partition = node_->findOrCreatePartition(
        record.tsdb_namespace(),
        record.stream_key(),
        SHA1Hash::fromHexString(record.partition_key()));

    partition->insertRecord(
        SHA1Hash::fromHexString(record.record_id()),
        Buffer(record.record_data().data(), record.record_data().size()));
  }

  res->setStatus(http::kStatusCreated);
}

void TSDBServlet::streamPartition(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    return;
  }

  String stream_key;
  if (!URI::getParam(params, "stream", &stream_key)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  size_t sample_mod = 0;
  size_t sample_idx = 0;
  String sample_str;
  if (URI::getParam(params, "sample", &sample_str)) {
    auto parts = StringUtil::split(sample_str, ":");

    if (parts.size() != 2) {
      res->setStatus(fnord::http::kStatusBadRequest);
      res->addBody("invalid ?sample=... parameter, format is <mod>:<idx>");
      res_stream->writeResponse(*res);
    }

    sample_mod = std::stoull(parts[0]);
    sample_idx = std::stoull(parts[1]);
  }


  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/octet-stream");
  res->addHeader("Connection", "close");
  res_stream->startResponse(*res);

  auto partition = node_->findPartition(
      tsdb_namespace,
      stream_key,
      SHA1Hash::fromHexString(partition_key));

  if (!partition.isEmpty()) {
    FNV<uint64_t> fnv;
    auto files = partition.get()->listFiles();

    for (const auto& f : files) {
      auto fpath = FileUtil::joinPaths(node_->dbPath(), f);
      sstable::SSTableReader reader(fpath);
      auto cursor = reader.getCursor();

      while (cursor->valid()) {
        uint64_t* key;
        size_t key_size;
        cursor->getKey((void**) &key, &key_size);
        if (key_size != SHA1Hash::kSize) {
          RAISE(kRuntimeError, "invalid row");
        }

        if (sample_mod == 0 ||
            (fnv.hash(key, key_size) % sample_mod == sample_idx)) {
          void* data;
          size_t data_size;
          cursor->getData(&data, &data_size);

          util::BinaryMessageWriter buf;
          if (data_size > 0) {
            buf.appendUInt64(data_size);
            buf.append(data, data_size);
            res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));
          }
          res_stream->waitForReader();
        }

        if (!cursor->next()) {
          break;
        }
      }
    }
  }

  util::BinaryMessageWriter buf;
  buf.appendUInt64(0);
  res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));

  res_stream->finishResponse();
}


void TSDBServlet::fetchPartitionInfo(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    return;
  }

  String stream_key;
  if (!URI::getParam(params, "stream", &stream_key)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(fnord::http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  auto partition = node_->findPartition(
      tsdb_namespace,
      stream_key,
      SHA1Hash::fromHexString(partition_key));

  PartitionInfo pinfo;
  pinfo.set_partition_key(partition_key);
  if (!partition.isEmpty()) {
    pinfo = partition.get()->partitionInfo();
  }

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/x-protobuf");
  res->addBody(*msg::encode(pinfo));
}

}

