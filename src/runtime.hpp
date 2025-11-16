#pragma once

extern "C" {
#define _Noreturn [[noreturn]] // NOLINT(bugprone-reserved-identifier)

#include "gc.h"
#include "runtime.h"
}
