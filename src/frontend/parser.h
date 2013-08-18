/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include <iostream>
#include "frontend.h"

namespace lean {
bool parse_commands(frontend & fe, std::istream & in,
                    std::ostream & out = std::cout, std::ostream & err = std::cerr,
                    bool use_exceptions = false);
expr parse_expr(frontend const & fe, std::istream & in);
}
