//
// Created by sexey on 28.08.2026.
//
module;
#include <winsock2.h>

module etsl.init;

namespace etsl
{
    /*@note
     * Сознательный отказ от освобождения: WSACleanup не вызывается никогда. Парный Cleanup потребовал бы
     * либо объекта с глобальным временем жизни, либо счётчика; при завершении процесса Winsock освобождает
     * ОС, поэтому цена утечки нулевая. Так же поступает libuv.
     *
     * Почему не function-local static с нетривиальным деструктором (как было раньше): на MinGW он требует
     * guard'а libstdc++ (__cxa_guard_acquire) и регистрации через __cxa_atexit, а те тянут за собой
     * EH/unwind/__cxa_demangle/winpthread — +134 КБ к любому бинарю (D16). Плоский static bool
     * инициализируется константой, живёт в .bss и не порождает ни guard'а, ни atexit-регистрации.
     *
     * Отличие от libuv: там once-проверка стоит на каждой публичной входной точке (дёшево, но держится на
     * предсказании ветвления), здесь — одна явная Initialize() в начале программы.
     *
     * Цена решения: потокобезопасности первого вызова нет. Конкурентный вызов может выполнить WSAStartup
     * дважды — сам WSAStartup потокобезопасен и считает вызовы, а Cleanup мы не зовём, поэтому последствий
     * нет, но формально это data race. В рамках ADR-7 v1 (один реактор — один поток) допустимо.
     */
    bool Initialize() noexcept
    {
        static bool initialized{false};
        if (initialized) {
            return true;
        }

        WSADATA wsaToken;
        if (WSAStartup(MAKEWORD(2, 2), &wsaToken) != ERROR_SUCCESS) {
            return false;
        }

        initialized = true;
        return true;
    }
}
