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

#include <deque>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

void register_in_tcl_interpreter(const std::string& command) {
    Tcl_Interp* interp = yosys_get_tcl_interp();
    std::string tcl_script = stringf("proc %s args { return [yosys %s {*}$args] }", command.c_str(), command.c_str());
    Tcl_Eval(interp, tcl_script.c_str());
}

// ============================================================================

struct Pin {
    RTLIL::Cell*    cell;   /// A cell pointer
    RTLIL::IdString port;   /// Cell port name
    int             bit;    /// Port bit index

    Pin (RTLIL::Cell* _cell, const RTLIL::IdString& _port, int _bit = 0) :
        cell (_cell),
        port (_port),
        bit  (_bit)
        {}

    Pin (const Pin& ref) = default;

    unsigned int hash() const {
        return mkhash_add(mkhash(cell->hash(), port.hash()), bit);
    };
};

bool operator == (const Pin& lhs, const Pin& rhs) {
    return (lhs.cell == rhs.cell) && \
           (lhs.port == rhs.port) && \
           (lhs.bit  == rhs.bit );
}

// ============================================================================

struct StartPoint {
    RTLIL::Cell* cell;
    int          level;

    StartPoint (RTLIL::Cell* _cell, int _level) :
        cell  (_cell),
        level (_level)
        {}

    StartPoint (const StartPoint& ref) = default;
};

// ============================================================================

struct QuicklogicMux : public Pass {

    /// Temporary SigBit to SigBit helper map.
    SigMap m_SigMap;

    std::unordered_set<RTLIL::Cell*> m_ProcessedMuxes;

    // ========================================================================

    QuicklogicMux () :
        Pass("quicklogic_mux", "Do magic with mux-inverter trees") {
            register_in_tcl_interpreter(pass_name);
        }    

    void help() YS_OVERRIDE {
        log("\n");
        log("    quicklogic_mux [selection]");
        log("\n");
        log("    Help goes here\n");
        log("\n");
    }   

    // ========================================================================
    
    void execute(std::vector<std::string> a_Args, RTLIL::Design* a_Design) YS_OVERRIDE {

        // All selected modules
        for (auto module : a_Design->selected_modules()) {
            log("Processing module '%s'\n", module->name.c_str());

            // Setup the SigMap
            m_SigMap.clear();
            m_SigMap.set(module);

            m_ProcessedMuxes.clear();

            // Identify startpoints
            log(" Identifying startpoints...\n");
            std::deque<StartPoint> startPoints;
            identifyStartingPoints(module, startPoints);

            // Process
            log(" Processing...\n");
            while (!startPoints.empty()) {

                // Get a point
                auto startPoint = startPoints.front();
                startPoints.pop_front();

                // Propagate inverter through a MUX if any
                processMux(startPoint.cell, startPoint.level);
                m_ProcessedMuxes.insert(startPoint.cell);

                // Identify new starting points upstream
                updateStartingPoints(startPoint.cell, startPoint.level, startPoints);
            }
        }

        // Clear the sigmap
        m_SigMap.clear();
    }

    // ========================================================================

    void processMux (RTLIL::Cell* a_Cell, int a_Level) {
        auto module = a_Cell->module;

        // Continue pushing inverters through the mux until there is none left
        while (1) {

            // Check if the mux is driving an inverter
            auto driver = Pin(a_Cell, RTLIL::escape_id("Y"));
            auto sinks  = getSinksForDriver(driver);

            // If there are more than one sinks then skip it
            if (sinks.size() != 1) {
                break;
            }

            auto sink = sinks[0];
            auto cell = sink.cell;

            // This is not an inverter, stop
            if (cell->type != RTLIL::escape_id("$_NOT_")) {
                break;
            }

            log("  Propagating inverter '%s' through '%s'\n",
                RTLIL::unescape_id(cell->name).c_str(),
                RTLIL::unescape_id(a_Cell->name).c_str());

            // Remove the inverter
            auto sigspec = cell->getPort(RTLIL::escape_id("Y"));
            a_Cell->setPort(RTLIL::escape_id("Y"), sigspec);
            
            module->remove(cell);

            // Add new inverters on the mux inputs
            for (RTLIL::IdString port : {RTLIL::escape_id("A"), RTLIL::escape_id("B")}) {
                cell = module->addCell(NEW_ID, RTLIL::escape_id("$_NOT_"));

                sigspec = a_Cell->getPort(port);
                cell->setPort(RTLIL::escape_id("A"), sigspec);

                auto wire = module->addWire(NEW_ID, 1);
                cell->setPort(RTLIL::escape_id("Y"), RTLIL::SigSpec(wire));
                a_Cell->setPort(port, RTLIL::SigSpec(wire));
            }
        }
    }

    // ========================================================================

    void identifyStartingPoints(RTLIL::Module* a_Module, std::deque<StartPoint>& startPoints) {

        for (auto cell : a_Module->selected_cells()) {
            auto cellType = RTLIL::unescape_id(cell->type);
            if (cellType == "$_MUX_") {

                auto driver = Pin(cell, RTLIL::escape_id("Y"));

                // Walk downstream
                while (1) {

                    // Get sinks, If the output is unconnected, skip the cell
                    auto sinks = getSinksForDriver(driver);
                    if (sinks.empty()) {
                        break;
                    }

                    // There are multiple sinks, make the cell a starting point
                    if (sinks.size() > 1) {
                        //log("StartPoint: %s\n", cell->name.c_str());
                        startPoints.push_back(StartPoint(cell, 0));
                        break;
                    }

                    // There is only one sink
                    auto sink  = sinks[0];
                    auto other = sink.cell;
                    log_assert(other != nullptr);

                    // We've hit another mux
                    if (other->type == RTLIL::escape_id("$_MUX_")) {

                        // If it is the select input the add a starting point
                        if (sink.port == RTLIL::escape_id("S")) {
                            //log("StartPoint: %s\n", cell->name.c_str());
                            startPoints.push_back(StartPoint(cell, 0));
                            break;
                        }
                        // Skip otherwise
                        else {
                            break;
                        }
                    }

                    // We've hit an inverter, go through it
                    else if (other->type == RTLIL::escape_id("$_NOT_")) {
                        driver = Pin(other, RTLIL::escape_id("Y"));
                    }

                    // We've hit something. Make the cell a starting point
                    else {
                        //log("StartPoint: %s\n", cell->name.c_str());
                        startPoints.push_back(StartPoint(cell, 0));
                        break;
                    }
                }
            }
        }
    }

    void updateStartingPoints(RTLIL::Cell* a_Cell, int a_Level, std::deque<StartPoint>& a_StartPoints) {
        auto module = a_Cell->module;

        log_assert(a_Cell->type == RTLIL::escape_id("$_MUX_"));

        // Check upstream connections of the mux.
        for (RTLIL::IdString port : {RTLIL::escape_id("A"), RTLIL::escape_id("B")}) {
            auto sink = Pin(a_Cell, port);

            // Walk upstream
            while (1) {

                // Get driver
                auto driver = getDriverForSink(sink);
                if (driver.cell == nullptr) {
                    break;
                }

                //log("d: %s %s\n", driver.cell->name.c_str(), driver.port.c_str());
                auto other = driver.cell;

                // We've hit another mux
                if (other->type == RTLIL::escape_id("$_MUX_")) {
                    int nextLevel = a_Level + 1;

                    // Maximum inverter popagation level reached, recurse
                    if (nextLevel >= 3) {
                        updateStartingPoints(other, 0, a_StartPoints);
                        break;
                    }
                    // Add a new start point
                    else {
                        if (!m_ProcessedMuxes.count(other)) {
                            //log("StartPoint: %s\n", other->name.c_str());
                            a_StartPoints.push_back(StartPoint(other, nextLevel));
                        }
                        break;
                    }
                }

                // We've hit an inverter, go through it
                else if (other->type == RTLIL::escape_id("$_NOT_")) {
                    sink = Pin(other, RTLIL::escape_id("A"));
                }

                // We've hit something, terminate
                else {
                    break;
                }
            }
        }
    }

    // ========================================================================

    RTLIL::SigSpec getSigSpecForPin (const Pin& a_Pin) {
        auto sigspec = a_Pin.cell->getPort(a_Pin.port);
        auto sigbits = sigspec.bits();

        return RTLIL::SigSpec(sigbits.at(a_Pin.bit));
    }

    std::vector<Pin> getSinksForDriver (const Pin& a_Driver) {
        auto module = a_Driver.cell->module;
        std::vector<Pin> sinks;

        // The driver has to be an output pin
        if (!a_Driver.cell->output(a_Driver.port)) {
            return sinks;
        }

        // Get the driver sigbit
        auto driverSigspec = a_Driver.cell->getPort(a_Driver.port);
        auto driverSigbit  = m_SigMap(driverSigspec.bits().at(a_Driver.bit));

        for (auto cell : module->selected_cells()) {
            for (auto conn : cell->connections()) {
                auto port    = conn.first;
                auto sigspec = conn.second;

                // Consider only sinks (inputs)
                if (!cell->input(port)) {
                    continue;
                }

                // Check all sigbits
                auto sigbits = sigspec.bits();
                for (size_t bit=0; bit<sigbits.size(); ++bit) {

                    auto sigbit = sigbits[bit];
                    if (!sigbit.wire) {
                        continue;
                    }

                    // Got one
                    sigbit = m_SigMap(sigbit);
                    if (sigbit == driverSigbit) {
                        sinks.push_back(Pin(cell, port, bit));
                    }
                }
            }
        }

        return sinks;
    }

    Pin getDriverForSink (const Pin& a_Sink) {
        auto module = a_Sink.cell->module;

        // The sink has to be an input pin
        log_assert(a_Sink.cell->input(a_Sink.port));

        // Get the sink sigbit
        auto sinkSigspec = a_Sink.cell->getPort(a_Sink.port);
        auto sinkSigbit  = m_SigMap(sinkSigspec.bits().at(a_Sink.bit));

        for (auto cell : module->selected_cells()) {
            for (auto conn : cell->connections()) {
                auto port    = conn.first;
                auto sigspec = conn.second;

                // Consider only drivers (outputs)
                if (!cell->output(port)) {
                    continue;
                }

                // Check all sigbits
                auto sigbits = sigspec.bits();
                for (size_t bit=0; bit<sigbits.size(); ++bit) {

                    auto sigbit = sigbits[bit];
                    if (!sigbit.wire) {
                        continue;
                    }

                    // Got one
                    sigbit = m_SigMap(sigbit);
                    if (sigbit == sinkSigbit) {
                        return Pin(cell, port, bit);
                    }
                }
            }
        }

        // Not found
        return Pin(nullptr, "", 0);
    }

} QuicklogicMux;

PRIVATE_NAMESPACE_END
