/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  Copyright (C) 2020  The Symbiflow Authors
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/rtlil.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

void register_in_tcl_interpreter(const std::string& command) {
    Tcl_Interp* interp = yosys_get_tcl_interp();
    std::string tcl_script = stringf("proc %s args { return [yosys %s {*}$args] }", command.c_str(), command.c_str());
    Tcl_Eval(interp, tcl_script.c_str());
}

struct GetCount : public Pass {

    GetCount () :
        Pass("get_count", "Returns count of various selected object types to the TCL interpreter") {
            register_in_tcl_interpreter(pass_name);
        }    

    void help() YS_OVERRIDE {
        log("\n");
        log("    get_count [selection]");
        log("\n");
        log("Help goes here.\n");
        log("\n");
    }
    
    void execute(std::vector<std::string> a_Args, RTLIL::Design* a_Design) YS_OVERRIDE {
        extra_args(a_Args, 1, a_Design);

        Tcl_Interp* tclInterp = yosys_get_tcl_interp();
        Tcl_Obj*    tclList = Tcl_NewListObj(0, NULL);

        // Count objects
        size_t moduleCount = 0;
        size_t cellCount   = 0;

        for (auto module : a_Design->selected_modules()) {
            for (auto cell : module->selected_cells()) {
                cellCount++;
            }

            moduleCount++;
        }

        size_t count = cellCount;
        std::string value = std::to_string(count);

        Tcl_Obj* tclStr = Tcl_NewStringObj(value.c_str(), value.size());
        Tcl_ListObjAppendElement(tclInterp, tclList, tclStr);

        Tcl_SetObjResult(tclInterp, tclList);
    }

} GetCount;

PRIVATE_NAMESPACE_END
