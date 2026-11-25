/* mcu_display.h -- see mcu_display.cpp */
#pragma once
#include <string>
bool mcu_display_route(const char *method, const char *path, const char *body, int *status, std::string *out,
                       const char **ctype);
