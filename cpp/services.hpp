#pragma once

#include <string>

void detected(const std::string &value);
void mqtt_worker(std::string host, int port, std::string topic);
void http_worker(int port);
