#include <iostream>

// Logging macros
#define LOG(level, format, ...) blog(level, "[obs-moq] " format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) LOG(LOG_DEBUG, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) LOG(LOG_INFO, format, ##__VA_ARGS__)
#define LOG_WARNING(format, ...) LOG(LOG_WARNING, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) LOG(LOG_ERROR, format, ##__VA_ARGS__)
