#pragma once

// Internal: serialises a Response to HTTP/1.1 wire format.

#include "ourweb/response.hpp"

#include <string>

namespace ourweb {

// Framing guarantees (FR-04.9):
//  * Content-Length always reflects the real body size; any application-set
//    Content-Length, Transfer-Encoding or Connection header is ignored
//  * "Connection: close" is always sent: the server closes after each response
//  * 204/304 responses carry no Content-Length and no body
//  * HEAD responses carry the Content-Length of the equivalent GET but no body
//  * a status code outside 200..599 is a programming error and is replaced by
//    a generic 500 so the client never sees a malformed status line
std::string serialize_response(const Response& response, bool is_head);

} // namespace ourweb
