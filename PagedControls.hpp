#pragma once
#ifndef PAGED_CONTROLS_H
#define PAGED_CONTROLS_H

// Back-compat shim. The implementation now lives in PagedMacroControls.hpp, which
// folds the original paging + soft-takeover + LED logic together with a macro
// (virtualization) routing layer. `pagedctl::PagedControls<P,K>` remains available
// as an alias for the no-routing configuration, so existing includes and call
// sites keep working unchanged.
#include "PagedMacroControls.hpp"

#endif // PAGED_CONTROLS_H
