/* mcu_http.h -- the ESP's robot + Servo Studio routes, on the MCU (mcu_http.cpp) */
#ifndef MCU_HTTP_H
#define MCU_HTTP_H
#include <stddef.h>
#include <string>
namespace mcu_http {
struct Response { int status; std::string ctype; std::string body; };
/* uri = path plus an optional ?query; body may be NULL */
Response handle(const char *method, const char *uri, const char *body, size_t body_len);
}
#endif
