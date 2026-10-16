#pragma once
// Leak mode (JIETUBA_LEAK_CHECK, scripts/check.ps1 -Mode Leak) force-includes this header into the
// test sources: QTEST_MAIN and QTEST_GUILESS_MAIN then run the test object three times and report
// the debug-heap blocks the second run allocated that the third did not free (tests/leak_check.cpp).
#include <QTest>
#include <functional>

namespace jietuba::leak_check {
// Runs the test object `make` returns through `exec` three times, then deletes it before the report.
// Returns the first failing pass's result, or 1 when project code leaked.
int run(int argc, char **argv, const std::function<QObject *()> &make, const std::function<int(QObject *, int, char **)> &exec);
}

#undef QTEST_MAIN_WRAPPER
#define QTEST_MAIN_WRAPPER(TestObject, ...) \
int main(int argc, char *argv[]) \
{ \
    QT_PREPEND_NAMESPACE(QTest::Internal::callInitMain)<TestObject>(); \
    __VA_ARGS__ \
    return jietuba::leak_check::run(argc, argv, []() -> QObject * { return new TestObject; }, \
        [](QObject *tc, int pass_argc, char **pass_argv) -> int { \
            QTEST_SET_MAIN_SOURCE_PATH \
            return QTest::qExec(tc, pass_argc, pass_argv); \
        }); \
}
