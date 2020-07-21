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
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

void register_in_tcl_interpreter(const std::string& command) {
    Tcl_Interp* interp = yosys_get_tcl_interp();
    std::string tcl_script = stringf("proc %s args { return [yosys %s {*}$args] }", command.c_str(), command.c_str());
    Tcl_Eval(interp, tcl_script.c_str());
}

struct IntegrateInv : public Pass {

    /// Temporary SigBit to SigBit helper map.
    SigMap m_SigMap;
    /// Map of SigBit objects to inverter cells.
    dict<RTLIL::SigBit, RTLIL::Cell*> m_InvMap;

    IntegrateInv () :
        Pass("integrateinv", "Integrates inverters ($_NOT_ cells) into ports with 'invertible_pin' attribute set") {
            register_in_tcl_interpreter(pass_name);
        }    

    void help() YS_OVERRIDE {
        log("\n");
        log("    integrateinv [selection]");
        log("\n");
        log("This pass integrates inverters into cells that have ports with the\n");
        log("'invertible_pin' attribute set. The attribute should contain name\n");
        log("of a parameter controlling the inversion. Whenever an inverter\n");
        log("of a parameter controlling the inversion. Whenever an inverter\n");
        log("\n");
        log("This pass is essentially the opposite of the 'extractinv' pass.\n");
        log("\n");
    }
    
    void execute(std::vector<std::string> a_Args, RTLIL::Design* a_Design) YS_OVERRIDE {
        log_header(a_Design, "Executing INTEGRATEINV pass (integrating pin inverters).\n");

		extra_args(a_Args, 1, a_Design);

        // Process modules
        for (auto module : a_Design->selected_modules()) {

            // Setup the SigMap
            m_SigMap.clear();
            m_SigMap.set(module);

            // Setup inverter map
            buildInverterMap(module);

            // Process cells
            for (auto cell : module->selected_cells()) {
                processCell(cell);
            }
        }

        // Clear the SigMap
        m_SigMap.clear();
    }

    void buildInverterMap (RTLIL::Module* a_Module) {
        m_InvMap.clear();

        for (auto cell : a_Module->cells()) {

            // Skip non-inverters
            if (cell->type != RTLIL::escape_id("$_NOT_")) {
                continue;
            }

            // Get output connection
            auto sigspec = cell->getPort(RTLIL::escape_id("Y"));
            auto sigbit  = m_SigMap(sigspec.bits().at(0));

            // Store
            log_assert(m_InvMap.count(sigbit) == 0);
            m_InvMap[sigbit] = cell;
        }
    }

    void processCell (RTLIL::Cell* a_Cell) {
        auto module = a_Cell->module;
        auto design = module->design;

        for (auto conn : a_Cell->connections()) {
            auto port    = conn.first;
		    auto sigspec = conn.second;

            // Consider only inputs.
            if (!a_Cell->input(port)) {
                continue;
            }

            // Get the cell module
            auto cellModule = design->module(a_Cell->type);
            if (!cellModule) {
                continue;
            }

            // Get wire.
			auto wire = cellModule->wire(port);
			if (!wire) {
    			continue;
            }

            // Check if the pin has an embedded inverter.
			auto it = wire->attributes.find(ID::invertible_pin);
			if (it == wire->attributes.end()) {
    			continue;
            }

            // Decode the parameter name.
			RTLIL::IdString paramName = RTLIL::escape_id(it->second.decode_string());
			RTLIL::Const    invMask;

			auto it2 = a_Cell->parameters.find(paramName);
	    	if (it2 == a_Cell->parameters.end()) {
		    	invMask = RTLIL::Const(0, sigspec.size());
            }
            else {
                invMask = RTLIL::Const(it2->second);
            }

            // Check width.
			if (invMask.size() != sigspec.size()) {
				log_error("The inversion parameter needs to be the same width as the port (%s.%s port %s parameter %s)",
                    log_id(module->name), log_id(a_Cell->type), log_id(port), log_id(paramName));
            }

            // Look for connected inverters
            auto sigbits = sigspec.bits();
            for (size_t bit=0; bit<sigbits.size(); ++bit) {

                auto sigbit = sigbits[bit];
                if (!sigbit.wire) {
                    continue;
                }

                sigbit = m_SigMap(sigbit);

                // Get the inverter if any
                if (!m_InvMap.count(sigbit)) {
                    continue;
                }
                auto inv = m_InvMap[sigbit];

                log("Integrating inverter %s into %s.%s\n",
                    log_id(inv->name), log_id(a_Cell->name), log_id(port));

                // Save the connection & remove the inverter
                sigbits[bit] = inv->getPort(RTLIL::escape_id("A"))[0];
                module->remove(inv);

                // Toggle the inversion bit in the mask
                if (invMask[bit] == RTLIL::State::S0) {
                    invMask[bit] = RTLIL::State::S1;
                }
                else if (invMask[bit] == RTLIL::State::S1) {
                    invMask[bit] = RTLIL::State::S0;
                }
                else {
                    log_error("The inversion parameter must contain only 0s and 1s (%s parameter %s)\n",
                        log_id(a_Cell->name), log_id(paramName));
                }
            }

            // Set the parameter
            log_debug("Updating inversion parameter %s.%s to %s\n",
                log_id(a_Cell->name), log_id(paramName), log_const(invMask));

            a_Cell->setParam(paramName, invMask);
        }
    }

} IntegrateInv;

PRIVATE_NAMESPACE_END
