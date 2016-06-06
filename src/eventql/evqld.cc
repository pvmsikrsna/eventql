/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <regex>
#include <sys/time.h>
#include <sys/resource.h>
#include "eventql/util/io/filerepository.h"
#include "eventql/util/io/fileutil.h"
#include "eventql/util/application.h"
#include "eventql/util/logging.h"
#include "eventql/util/random.h"
#include "eventql/util/assets.h"
#include "eventql/util/thread/eventloop.h"
#include "eventql/util/thread/threadpool.h"
#include "eventql/util/thread/FixedSizeThreadPool.h"
#include "eventql/util/wallclock.h"
#include "eventql/util/VFS.h"
#include "eventql/util/rpc/ServerGroup.h"
#include "eventql/util/rpc/RPC.h"
#include "eventql/util/rpc/RPCClient.h"
#include "eventql/util/cli/flagparser.h"
#include "eventql/util/json/json.h"
#include "eventql/util/json/jsonrpc.h"
#include "eventql/util/http/httprouter.h"
#include "eventql/util/http/httpserver.h"
#include "eventql/util/http/VFSFileServlet.h"
#include "eventql/util/io/FileLock.h"
#include "eventql/util/stats/statsdagent.h"
#include "eventql/io/sstable/SSTableServlet.h"
#include "eventql/util/mdb/MDB.h"
#include "eventql/util/mdb/MDBUtil.h"
#include "eventql/transport/http/api_servlet.h"
#include "eventql/db/TableConfig.pb.h"
#include "eventql/db/table_service.h"
#include "eventql/db/metadata_coordinator.h"
#include "eventql/db/metadata_replication.h"
#include "eventql/db/metadata_service.h"
#include "eventql/transport/http/rpc_servlet.h"
#include "eventql/db/ReplicationWorker.h"
#include "eventql/db/LSMTableIndexCache.h"
#include "eventql/db/CompactionWorker.h"
#include "eventql/server/sql/sql_engine.h"
#include "eventql/transport/http/default_servlet.h"
#include "eventql/sql/defaults.h"
#include "eventql/config/config_directory.h"
#include "eventql/config/config_directory_legacy.h"
#include "eventql/config/config_directory_zookeeper.h"
#include "eventql/transport/http/status_servlet.h"
#include "eventql/server/sql/scheduler.h"
#include "eventql/auth/client_auth.h"
#include "eventql/auth/client_auth_trust.h"
#include "eventql/auth/client_auth_legacy.h"
#include "eventql/auth/internal_auth.h"
#include "eventql/auth/internal_auth_trust.h"
#include <jsapi.h>
#include "eventql/mapreduce/mapreduce_preludejs.cc"

#include "eventql/eventql.h"
using namespace eventql;

thread::EventLoop ev;

namespace js {
void DisableExtraThreads();
}

int main(int argc, const char** argv) {
  Application::init();
  __eventql_mapreduce_prelude_js.registerAsset();

  cli::FlagParser flags;

  flags.defineFlag(
      "help",
      cli::FlagParser::T_SWITCH,
      false,
      "?",
      NULL,
      "help",
      "<help>");

  flags.defineFlag(
      "version",
      cli::FlagParser::T_SWITCH,
      false,
      "V",
      NULL,
      "print version",
      "<switch>");

  flags.defineFlag(
      "listen",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "listen",
      "<host:port>");

  flags.defineFlag(
      "cachedir",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "cachedir path",
      "<path>");

  flags.defineFlag(
      "datadir",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "datadir path",
      "<path>");

#ifndef eventql_HAS_ASSET_BUNDLE
  flags.defineFlag(
      "asset_path",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      "src/",
      "assets path",
      "<path>");
#endif

  flags.defineFlag(
      "indexbuild_threads",
      cli::FlagParser::T_INTEGER,
      false,
      NULL,
      "2",
      "number of indexbuild threads",
      "<num>");

  flags.defineFlag(
      "config_backend",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "backend",
      "<backend>");

  flags.defineFlag(
      "client_auth_backend",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "backend",
      "<backend>");

  flags.defineFlag(
      "legacy_auth_secret",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "secret",
      "<secret>");

  flags.defineFlag(
      "legacy_master_addr",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "url",
      "<addr>");

  flags.defineFlag(
      "zookeeper_addr",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "url",
      "<addr>");

  flags.defineFlag(
      "cluster",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "name",
      "<name>");

  flags.defineFlag(
      "create_cluster",
      cli::FlagParser::T_SWITCH,
      false,
      NULL,
      NULL,
      "switch",
      "<switch>");

  flags.defineFlag(
      "join",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "url",
      "<name>");

  flags.defineFlag(
      "loglevel",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      "INFO",
      "loglevel",
      "<level>");

  flags.defineFlag(
      "daemonize",
      cli::FlagParser::T_SWITCH,
      false,
      NULL,
      NULL,
      "daemonize",
      "<switch>");

  flags.defineFlag(
      "pidfile",
      cli::FlagParser::T_STRING,
      false,
      NULL,
      NULL,
      "pidfile",
      "<path>");

  flags.defineFlag(
      "log_to_syslog",
      cli::FlagParser::T_SWITCH,
      false,
      NULL,
      NULL,
      "log to syslog",
      "<switch>");

  flags.defineFlag(
      "nolog_to_stderr",
      cli::FlagParser::T_SWITCH,
      false,
      NULL,
      NULL,
      "don't log to stderr",
      "<switch>");

  flags.parseArgv(argc, argv);

  if (!flags.isSet("nolog_to_stderr") && !flags.isSet("daemonize")) {
    Application::logToStderr("evqld");
  }

  if (flags.isSet("log_to_syslog")) {
    Application::logToSyslog("evqld");
  }

  Logger::get()->setMinimumLogLevel(
      strToLogLevel(flags.getString("loglevel")));

  /* print help */
  if (flags.isSet("help") || flags.isSet("version")) {
    auto stdout_os = OutputStream::getStdout();
    stdout_os->write(
        StringUtil::format(
            "EventQL $0 ($1)\n"
            "Copyright (c) 2016, zScale Techology GmbH. All rights reserved.\n\n",
            kVersionString,
            kBuildID));
  }

  if (flags.isSet("version")) {
    return 0;
  }

  if (flags.isSet("help")) {
    auto stdout_os = OutputStream::getStdout();
    stdout_os->write(
        "Usage: $ evqld [OPTIONS]\n"
        "  -?, --help              Display this help text and exit\n"
        "  -V, --version           Display the version of this binary and exit\n"
    );
    return 0;
  }

  if (flags.isSet("daemonize")) {
    Application::daemonize();
  }

  ScopedPtr<FileLock> pidfile_lock;
  if (flags.isSet("pidfile")) {
    pidfile_lock = mkScoped(new FileLock(flags.getString("pidfile")));
    pidfile_lock->lock(false);

    auto pidfile = File::openFile(
        flags.getString("pidfile"),
        File::O_WRITE | File::O_CREATEOROPEN | File::O_TRUNCATE);

    pidfile.write(StringUtil::toString(getpid()));
  }

  /* conf */
  //auto conf_data = FileUtil::read(flags.getString("conf"));
  //auto conf = msg::parseText<eventql::TSDBNodeConfig>(conf_data);

  /* thread pools */
  thread::CachedThreadPool tpool(
      thread::ThreadPoolOptions {
        .thread_name = Some(String("z1d-httpserver"))
      },
      8);

  /* listen addr */
  String listen_host;
  int listen_port;
  {
    auto listen_str = flags.getString("listen");
    std::smatch m;
    std::regex listen_regex("([0-9a-zA-Z-_.]+):([0-9]+)");
    if (std::regex_match(listen_str, m, listen_regex)) {
      listen_host = m[1];
      listen_port = std::stoi(m[2]);
    } else {
      logFatal("evqld", "invalid listen address: $0", listen_str);
      return 1;
    }
  }

  /* http */
  http::HTTPRouter http_router;
  http::HTTPServer http_server(&http_router, &ev);
  http_server.listen(listen_port);
  http::HTTPConnectionPool http(&ev, &z1stats()->http_client_stats);

  Option<String> server_name;
  if (flags.isSet("join")) {
    server_name = Some(flags.getString("join"));
  }

  /* data dirdirectory */
  if (!FileUtil::exists(flags.getString("datadir"))) {
    logFatal("evqld", "data dir not found: " + flags.getString("datadir"));
    return 1;
  }

  String node_name = "__anonymous";
  if (flags.isSet("join")) {
    node_name = flags.getString("join");
  }

  auto tsdb_dir = FileUtil::joinPaths(
      flags.getString("datadir"),
      "data/" + node_name);

  if (!FileUtil::exists(tsdb_dir)) {
    FileUtil::mkdir_p(tsdb_dir);
  }

  /* config dir */
  ScopedPtr<ConfigDirectory> config_dir;
  if (flags.getString("config_backend") == "zookeeper") {
    config_dir.reset(
        new ZookeeperConfigDirectory(
            flags.getString("zookeeper_addr"),
            flags.getString("cluster"),
            server_name,
            flags.getString("listen")));
  } else {
    logFatal("evqld", "invalid config backend: " + flags.getString("config_backend"));
  }

  /* client auth */
  ScopedPtr<eventql::ClientAuth> client_auth;
  if (flags.getString("client_auth_backend") == "trust") {
    client_auth.reset(new TrustClientAuth());
  } else if (flags.getString("client_auth_backend") == "legacy") {
    client_auth.reset(new LegacyClientAuth(flags.getString("legacy_auth_secret")));
  } else {
    logFatal("evqld", "invalid client auth backend: " + flags.getString("client_auth_backend"));
  }

  /* internal auth */
  ScopedPtr<eventql::InternalAuth> internal_auth;
  internal_auth.reset(new TrustInternalAuth());

  /* metadata service */
  auto metadata_dir = FileUtil::joinPaths(
      flags.getString("datadir"),
      "metadata/" + node_name);
  if (!FileUtil::exists(metadata_dir)) {
    FileUtil::mkdir_p(metadata_dir);
  }

  eventql::MetadataStore metadata_store(metadata_dir);
  eventql::MetadataService metadata_service(config_dir.get(), &metadata_store);

  /* spidermonkey javascript runtime */
  JS_Init();
  js::DisableExtraThreads();

  try {
    {
      auto rc = config_dir->start();
      if (!rc.isSuccess()) {
        logFatal("evqld", "Can't connect to config backend: $0", rc.message());
        return 1;
      }
    }

    /* clusterconfig */
    auto cluster_config = config_dir->getClusterConfig();
    logInfo(
        "eventql",
        "Starting with cluster config: $0",
        cluster_config.DebugString());

    /* tsdb */
    auto repl_scheme = RefPtr<eventql::ReplicationScheme>(
          new eventql::DHTReplicationScheme(cluster_config, server_name));

    auto trash_dir = FileUtil::joinPaths(flags.getString("datadir"), "trash");
    if (!FileUtil::exists(trash_dir)) {
      FileUtil::mkdir(trash_dir);
    }

    FileLock server_lock(FileUtil::joinPaths(tsdb_dir, "__lock"));
    server_lock.lock();

    eventql::ServerCfg cfg;
    cfg.db_path = tsdb_dir;
    cfg.repl_scheme = repl_scheme;
    cfg.config_directory = config_dir.get();
    cfg.idx_cache = mkRef(new LSMTableIndexCache(tsdb_dir));
    cfg.metadata_store = &metadata_store;

    eventql::PartitionMap partition_map(&cfg);
    eventql::TableService table_service(
        config_dir.get(),
        &partition_map,
        repl_scheme.get(),
        &ev,
        &z1stats()->http_client_stats);

    eventql::ReplicationWorker tsdb_replication(
        repl_scheme.get(),
        &partition_map,
        &http);

    eventql::RPCServlet tsdb_servlet(
        &table_service,
        &metadata_service,
        flags.getString("cachedir"));

    http_router.addRouteByPrefixMatch("/tsdb", &tsdb_servlet, &tpool);
    http_router.addRouteByPrefixMatch("/rpc", &tsdb_servlet, &tpool);

    eventql::CompactionWorker cstable_index(
        &partition_map,
        flags.getInt("indexbuild_threads"));

    /* metadata replication */
    ScopedPtr<MetadataReplication> metadata_replication;
    if (!server_name.isEmpty()) {
      metadata_replication.reset(
          new MetadataReplication(
              config_dir.get(),
              server_name.get(),
              &metadata_store));
    }

    /* sql */
    RefPtr<csql::Runtime> sql;
    {
      auto symbols = mkRef(new csql::SymbolTable());
      csql::installDefaultSymbols(symbols.get());
      sql = mkRef(new csql::Runtime(
          thread::ThreadPoolOptions {
            .thread_name = Some(String("z1d-sqlruntime"))
          },
          symbols,
          new csql::QueryBuilder(
              new csql::ValueExpressionBuilder(symbols.get())),
          new csql::QueryPlanBuilder(
              csql::QueryPlanBuilderOptions{},
              symbols.get()),
          mkScoped(
              new Scheduler(
                  &partition_map,
                  config_dir.get(),
                  internal_auth.get(),
                  repl_scheme.get()))));

      sql->setCacheDir(flags.getString("cachedir"));
      sql->symbols()->registerFunction("z1_version", &z1VersionExpr);
    }

    /* more services */
    eventql::SQLService sql_service(
        sql.get(),
        &partition_map,
        config_dir.get(),
        repl_scheme.get(),
        internal_auth.get(),
        &table_service);

    eventql::LogfileService logfile_service(
        config_dir.get(),
        internal_auth.get(),
        &table_service,
        &partition_map,
        repl_scheme.get(),
        sql.get());

    eventql::MapReduceService mapreduce_service(
        config_dir.get(),
        internal_auth.get(),
        &table_service,
        &partition_map,
        repl_scheme.get(),
        flags.getString("cachedir"));

    /* open tables */
    config_dir->setTableConfigChangeCallback(
        [&partition_map, &tsdb_replication] (const TableDefinition& tbl) {
      Set<SHA1Hash> affected_partitions;
      partition_map.configureTable(tbl, &affected_partitions);

      for (const auto& partition_id : affected_partitions) {
        auto partition = partition_map.findPartition(
            tbl.customer(),
            tbl.table_name(),
            partition_id);

        tsdb_replication.enqueuePartition(partition.get(), 0);
      }
    });

    config_dir->listTables([&partition_map] (const TableDefinition& tbl) {
      partition_map.configureTable(tbl);
    });

    eventql::AnalyticsServlet analytics_servlet(
        flags.getString("cachedir"),
        internal_auth.get(),
        client_auth.get(),
        internal_auth.get(),
        sql.get(),
        &table_service,
        config_dir.get(),
        &partition_map,
        &sql_service,
        &logfile_service,
        &mapreduce_service,
        &table_service);

    eventql::StatusServlet status_servlet(
        &cfg,
        &partition_map,
        config_dir.get(),
        http_server.stats(),
        &z1stats()->http_client_stats);

    eventql::DefaultServlet default_servlet;

    http_router.addRouteByPrefixMatch("/api/", &analytics_servlet, &tpool);
    http_router.addRouteByPrefixMatch("/zstatus", &status_servlet, &tpool);
    http_router.addRouteByPrefixMatch("/", &default_servlet);

    auto rusage_t = std::thread([] () {
      for (;; usleep(1000000)) {
        logDebug(
            "eventql",
            "Using $0MB of memory (peak $1)",
            Application::getCurrentMemoryUsage() / 1000000.0,
            Application::getPeakMemoryUsage() / 1000000.0);
      }
    });

    rusage_t.detach();

    Application::setCurrentThreadName("z1d");

    partition_map.open();
    if (metadata_replication.get()) {
      metadata_replication->start();
    }

    ev.run();

    if (metadata_replication.get()) {
      metadata_replication->stop();
    }
  } catch (const StandardException& e) {
    logAlert("eventql", e, "FATAL ERROR");
  }

  logInfo("eventql", "Exiting...");
  config_dir->stop();
  JS_ShutDown();

  if (flags.isSet("pidfile")) {
    pidfile_lock.reset(nullptr);
    FileUtil::rm(flags.getString("pidfile"));
  }

  exit(0);
}

