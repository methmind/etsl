//
// Created by sexey on 04.08.2026.
//

#ifndef ETSL_NONCOPYABLE_H
#define ETSL_NONCOPYABLE_H

#define ETSL_NON_COPYABLE(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete; \

#define ETSL_NON_MOVABLE(ClassName) \
    ClassName(ClassName&&) = delete; \
    ClassName& operator=(ClassName&&) = delete;

#define ETSL_NON_COPYABLE_NON_MOVABLE(ClassName) \
    ETSL_NON_COPYABLE(ClassName); \
    ETSL_NON_MOVABLE(ClassName);

#define ETSL_DEFAULT_NON_COPYABLE_NON_MOVABLE(ClassName) \
    ClassName() = default; \
    ~ClassName() = default; \
    ETSL_NON_COPYABLE_NON_MOVABLE(ClassName)

#endif //ETSL_NONCOPYABLE_H
