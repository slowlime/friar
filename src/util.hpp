#pragma once

namespace friar::util {
    template<class... Ts>
    struct overloaded : Ts... {
        using Ts::operator()...;
    };
}
