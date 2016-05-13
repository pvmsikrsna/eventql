/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#pragma once
#include <eventql/util/stdtypes.h>
#include <eventql/util/http/httpservice.h>
#include <eventql/AnalyticsSession.pb.h>
#include <eventql/AnalyticsAuth.h>

using namespace stx;
namespace zbase {

class HTTPAuth {
public:

  static const char kSessionCookieKey[];
  static const uint64_t kSessionLifetimeMicros;

  static Option<AnalyticsSession> authenticateRequest(
      const http::HTTPRequest& request,
      AnalyticsAuth* auth);

};

}