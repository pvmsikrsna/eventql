/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "stx/io/filerepository.h"
#include "stx/io/fileutil.h"
#include "stx/application.h"
#include "stx/logging.h"
#include "stx/random.h"
#include "stx/thread/eventloop.h"
#include "stx/thread/threadpool.h"
#include "stx/thread/FixedSizeThreadPool.h"
#include "stx/wallclock.h"
#include "stx/VFS.h"
#include "stx/rpc/ServerGroup.h"
#include "stx/rpc/RPC.h"
#include "stx/rpc/RPCClient.h"
#include "stx/cli/flagparser.h"
#include "stx/json/json.h"
#include "stx/json/jsonrpc.h"
#include "stx/http/httprouter.h"
#include "stx/http/httpserver.h"
#include "stx/http/VFSFileServlet.h"
#include "brokerd/FeedService.h"
#include "brokerd/RemoteFeedFactory.h"
#include "brokerd/RemoteFeedReader.h"
#include "stx/stats/statsdagent.h"
#include "sstable/SSTableServlet.h"
#include "fnord-logtable/LogTableServlet.h"
#include "fnord-logtable/TableRepository.h"
#include "fnord-logtable/TableJanitor.h"
#include "fnord-logtable/TableReplication.h"
#include "fnord-afx/ArtifactReplication.h"
#include "fnord-afx/ArtifactIndexReplication.h"
#include "fnord-logtable/NumericBoundsSummary.h"
#include "stx/mdb/MDB.h"
#include "stx/mdb/MDBUtil.h"
#include "stx/protobuf/MessageSchema.h"
#include "tsdb/TSDBNode.h"
#include "tsdb/TSDBServlet.h"
#include "common.h"
#include "schemas.h"
#include "ModelReplication.h"
#include "JoinedSessionViewer.h"

using namespace fnord;

std::atomic<bool> shutdown_sig;
fnord::thread::EventLoop ev;

void quit(int n) {
  shutdown_sig = true;
  fnord::logInfo("cm.chunkserver", "Shutting down...");
  // FIXPAUL: wait for http server stop...
  ev.shutdown();
}

int main(int argc, const char** argv) {
  fnord::Application::init();
  fnord::Application::logToStderr();

  /* shutdown hook */
  shutdown_sig = false;
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  sa.sa_handler = quit;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGQUIT, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  fnord::cli::FlagParser flags;

  flags.defineFlag(
      "http_port",
      fnord::cli::FlagParser::T_INTEGER,
      false,
      NULL,
      "8000",
      "Start the public http server on this port",
      "<port>");

  flags.defineFlag(
      "datadir",
      cli::FlagParser::T_STRING,
      true,
      NULL,
      NULL,
      "datadir path",
      "<path>");

  flags.defineFlag(
      "replicate_to",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "url",
      "<url>");

  flags.defineFlag(
      "loglevel",
      fnord::cli::FlagParser::T_STRING,
      false,
      NULL,
      "INFO",
      "loglevel",
      "<level>");

  flags.parseArgv(argc, argv);

  Logger::get()->setMinimumLogLevel(
      strToLogLevel(flags.getString("loglevel")));

  /* args */
  auto dir = flags.getString("datadir");
  auto repl_targets = flags.getStrings("replicate_to");

  /* load schemas */
  msg::MessageSchemaRepository schemas;
  loadDefaultSchemas(&schemas);

  /* start http server and worker pools */
  fnord::thread::ThreadPool tpool;
  http::HTTPConnectionPool http(&ev);
  fnord::http::HTTPRouter http_router;
  fnord::http::HTTPServer http_server(&http_router, &ev);
  http_server.listen(flags.getInt("http_port"));



  JoinedSessionViewer jsessviewer;
  http_router.addRouteByPrefixMatch("/sessviewer", &jsessviewer);


  auto repl_scheme = mkRef(new dproc::FixedReplicationScheme());
  for (const auto& r : repl_targets) {
    repl_scheme->addHost(r);
  }

  tsdb::TSDBNode tsdb_node(dir + "/tsdb", repl_scheme.get(), &http);

  {
    tsdb::TableConfig config;
    config.set_table_name("joined_sessions.");
    config.set_max_sstable_size(1024 * 1024 * 512);
    config.set_compaction_interval(1800 * kMicrosPerSecond);
    config.set_partitioner(tsdb::TIME_WINDOW);
    config.set_partition_window(3600 * 4 * kMicrosPerSecond);
    tsdb_node.configurePrefix("dawanda", config);
  }

  {
    tsdb::TableConfig config;
    config.set_table_name("metricd.sensors.");
    config.set_max_sstable_size(1024 * 1024 * 512);
    config.set_compaction_interval(10 * kMicrosPerSecond);
    config.set_partitioner(tsdb::TIME_WINDOW);
    config.set_partition_window(600 * kMicrosPerSecond);
    tsdb_node.configurePrefix("dawanda", config);
  }

  {
    tsdb::TableConfig config;
    config.set_table_name("metricd.metrics.");
    config.set_max_sstable_size(1024 * 1024 * 512);
    config.set_compaction_interval(10 * kMicrosPerSecond);
    config.set_partitioner(tsdb::TIME_WINDOW);
    config.set_partition_window(600 * kMicrosPerSecond);
    tsdb_node.configurePrefix("dawanda", config);
  }

  tsdb::TSDBServlet tsdb_servlet(&tsdb_node);
  http_router.addRouteByPrefixMatch("/tsdb", &tsdb_servlet, &tpool);

  tsdb_node.start();
  ev.run();

  tsdb_node.stop();
  fnord::logInfo("cm.chunkserver", "Exiting...");

  exit(0);
}
