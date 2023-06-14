/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *  2022 Josh Kang <mkang@eecs.berkeley.edu>
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

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/cellaigs.h"
#include "kernel/log.h"
#include <string>
#include <map>
#include <vector>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct GmlWriter
{
	std::ostream &f;
	bool use_selection;
	bool aig_mode;
	bool compat_int_mode;

	Design *design;
	Module *module;

	SigMap sigmap;
	int sigidcounter;
	dict<SigBit, string> sigids;
	pool<Aig> aig_models;

	std::map<std::string, std::vector<int>> sourceBitToIdxMap = {};
	std::map<std::string, std::vector<int>> targetBitToIdxMap = {};

	GmlWriter(std::ostream &f, bool use_selection, bool aig_mode, bool compat_int_mode) :
			f(f), use_selection(use_selection), aig_mode(aig_mode),
			compat_int_mode(compat_int_mode) { }

	string get_string(string str)
	{
		string newstr = "\"";
		for (char c : str) {
			if (c == '\\')
				newstr += "\\\\";
			else if (c == '"')
				newstr += "\\\"";
			else if (c == '\b')
				newstr += "\\b";
			else if (c == '\f')
				newstr += "\\f";
			else if (c == '\n')
				newstr += "\\n";
			else if (c == '\r')
				newstr += "\\r";
			else if (c == '\t')
				newstr += "\\t";
			else if (c < 0x20)
				newstr += stringf("\\u%04X", c);
			else
				newstr += c;
		}
		return newstr + "\"";
	}

	string get_name(IdString name)
	{
		return get_string(RTLIL::unescape_id(name));
	}

	string get_bits(SigSpec sig)
	{
		bool first = true;
		string str = "[";
		for (auto bit : sigmap(sig)) {
			str += first ? " " : ", ";
			first = false;
			if (sigids.count(bit) == 0) {
				string &s = sigids[bit];
				if (bit.wire == nullptr) {
					if (bit == State::S0) s = "\"0\"";
					else if (bit == State::S1) s = "\"1\"";
					else if (bit == State::Sz) s = "\"z\"";
					else s = "\"x\"";
				} else
					s = stringf("%d", sigidcounter++);
			}
			str += sigids[bit];
		}
		return str + " ]";
	}

	int updateIndexMap(SigSpec sig, int idx, bool updateSource)
	{
		int iter = 0;
		for (auto bit : sigmap(sig)) {
			if (sigids.count(bit) == 0) {
				string &s = sigids[bit];
				if (bit.wire == nullptr) {
					if (bit == State::S0) s = "\"0\"";
					else if (bit == State::S1) s = "\"1\"";
					else if (bit == State::Sz) s = "\"z\"";
					else s = "\"x\"";
				} else
					s = stringf("%d", sigidcounter++);
			}
			if (updateSource)
				sourceBitToIdxMap[sigids[bit]].push_back(idx);
			else 
				targetBitToIdxMap[sigids[bit]].push_back(idx);
			iter++;
		}
		return iter;
	}
	

	vector<string> get_bits_vector(SigSpec sig)
	{
		vector<string> sigvector;
		sigvector.reserve(get_bits(sig).size());
		for (auto bit : sigmap(sig)) {
			sigvector.push_back(sigids[bit]);
		}
		return sigvector;
	}

	void write_module(Module *module_)
	{
		module = module_;
		log_assert(module->design == design);
		sigmap.set(module);
		sigids.clear();

		// reserve 0 and 1 to avoid confusion with "0" and "1"
		sigidcounter = 2;

		if (module->has_processes()) {
			log_error("Module %s contains processes, which are not supported by GML backend (run `proc` first).\n", log_id(module));
		}
		// Write nodes (1) PI and POs
		int idCounter = 2;

		// Create nodes from all wires
		for (auto n : module->ports) {
			Wire *w = module->wire(n);
			if (use_selection && !module->selected(w))
				continue;
			f << stringf("          node [  id  %d    label  %s \n", idCounter, get_name(n).c_str());
			//f << stringf("          node [  id  %d    label  %s \n", idCounter, get_name(n.first).c_str());
			f << stringf("              type	\"%s\"\n", w->port_input ? w->port_output ? "inout" : "input" : "output");
			f << stringf("          ]\n");
			idCounter += updateIndexMap(w, idCounter, w->port_input); // TODO :handle inout cases
		}

		// Create nodes for each cell and create edges between cell connections 
		for (auto c : module->cells()) {
			if (use_selection && !module->selected(c))
				continue;
			
			f << stringf("          node [  id  %d    label  %s \n", idCounter, get_name(c->name).c_str());
			f << stringf("              type	%s\n", get_name(c->type).c_str());
			f << stringf("          ]\n");
			
			// update mappings; add information about each of the connecting wires to this cell
			for (auto &conn : c->connections()) {
				if (c->input(conn.first)){
					updateIndexMap(conn.second, idCounter, false); //if input connection, the wire's target is this cell 
				}
				if (c->output(conn.first)){
					updateIndexMap(conn.second, idCounter, true);
				}
			}
			idCounter += 1;
		}
		// for (auto w : module->wires()) {
		// 	if (use_selection && !module->selected(w))
		// 		continue;
		// 	vector<string> bits = get_bits_vector(w);
		// 	for (auto &bit : bits) {
		// 		vector<int> sources = sourceBitToIdxMap[bit];
		// 		vector<int> targets = targetBitToIdxMap[bit];
		// 		for (auto &s : sources) {
		// 			for (auto &t : targets) {
		// 				f << stringf("          edge [    source  %d    target  %d    ] \n", s, t);
		// 			}
		// 		}
		// 	}
		// }
		// for (auto c : module->cells()) {
		// 	if (use_selection && !module->selected(c))
		// 		continue;
		// 	for (auto &conn : c->connections()) {
		// 		if (c->input(conn.first)){
		// 		}
		// 		if (c->output(conn.first)){
		// 		}
		// 	}
		for (auto w : module->wires()) {
			if (use_selection && !module->selected(w))
				continue;
			// f << stringf("          working on wire : %s \n", get_name(w->name).c_str());
			vector<string> bits = get_bits_vector(w);
			int prevSource = 0;
			int prevTarget = 0;
			for (auto &bit : bits) {
				// f << stringf("          working on bit : %s \n", bit.c_str());
				vector<int> sources = sourceBitToIdxMap[bit];
				vector<int> targets = targetBitToIdxMap[bit];
				for (auto &s : sources) {
					for (auto &t : targets) {
						if (prevSource == s && prevTarget == t)
							continue;
						f << stringf("          edge [    source  %d    target  %d    ] \n", s, t);
						prevSource = s;
						prevTarget = t;
					}
				}
			}
		}
	}


    // Top level function to write design to gml
	void write_design(Design *design_)
	{
		design = design_;
		design->sort();

		//f << stringf("  # GML created by: %s,\n", get_string(yosys_version_str).c_str());
		f << stringf("graph [\n");
		f << stringf("    multigraph 1\n");
		// f << stringf("  \"modules\": {\n");
		vector<Module*> modules = use_selection ? design->selected_modules() : design->modules();
		// bool first_module = true;
		for (auto mod : modules) {
			// if (!first_module)
			// 	f << stringf(",\n");
			write_module(mod);
			// first_module = false;
		}
		f << stringf("]");
	}
};

struct GmlBackend : public Backend {
	GmlBackend() : Backend("gml", "write design to a GML file") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_gml [options] [filename]\n");
		log("\n");
		log("Write a gml netlist of the current design.\n");
		log("\n");
		log("    -aig\n");
		log("        include AIG models for the different gate types\n");
		log("\n");
		log("    -compat-int\n");
		log("        emit 32-bit or smaller fully-defined parameter values directly\n");
		log("        as JSON numbers (for compatibility with old parsers)\n");
		log("\n");
		log("\n");
		log("The general syntax of the JSON output created by this command is as follows:\n");
		log("\n");
		log("    {\n");
		log("      \"creator\": \"Yosys <version info>\",\n");
		log("      \"modules\": {\n");
		log("        <module_name>: {\n");
		log("          \"attributes\": {\n");
		log("            <attribute_name>: <attribute_value>,\n");
		log("            ...\n");
		log("          },\n");
		log("          \"parameter_default_values\": {\n");
		log("            <parameter_name>: <parameter_value>,\n");
		log("            ...\n");
		log("          },\n");
		log("          \"ports\": {\n");
		log("            <port_name>: <port_details>,\n");
		log("            ...\n");
		log("          },\n");
		log("          \"cells\": {\n");
		log("            <cell_name>: <cell_details>,\n");
		log("            ...\n");
		log("          },\n");
		log("          \"memories\": {\n");
		log("            <memory_name>: <memory_details>,\n");
		log("            ...\n");
		log("          },\n");
		log("          \"netnames\": {\n");
		log("            <net_name>: <net_details>,\n");
		log("            ...\n");
		log("          }\n");
		log("        }\n");
		log("      },\n");
		log("      \"models\": {\n");
		log("        ...\n");
		log("      },\n");
		log("    }\n");
		log("\n");
		log("Where <port_details> is:\n");
		log("\n");
		log("    {\n");
		log("      \"direction\": <\"input\" | \"output\" | \"inout\">,\n");
		log("      \"bits\": <bit_vector>\n");
		log("      \"offset\": <the lowest bit index in use, if non-0>\n");
		log("      \"upto\": <1 if the port bit indexing is MSB-first>\n");
		log("      \"signed\": <1 if the port is signed>\n");
		log("    }\n");
		log("\n");
		log("The \"offset\" and \"upto\" fields are skipped if their value would be 0.");
		log("They don't affect connection semantics, and are only used to preserve original");
		log("HDL bit indexing.");
		log("And <cell_details> is:\n");
		log("\n");
		log("    {\n");
		log("      \"hide_name\": <1 | 0>,\n");
		log("      \"type\": <cell_type>,\n");
		log("      \"model\": <AIG model name, if -aig option used>,\n");
		log("      \"parameters\": {\n");
		log("        <parameter_name>: <parameter_value>,\n");
		log("        ...\n");
		log("      },\n");
		log("      \"attributes\": {\n");
		log("        <attribute_name>: <attribute_value>,\n");
		log("        ...\n");
		log("      },\n");
		log("      \"port_directions\": {\n");
		log("        <port_name>: <\"input\" | \"output\" | \"inout\">,\n");
		log("        ...\n");
		log("      },\n");
		log("      \"connections\": {\n");
		log("        <port_name>: <bit_vector>,\n");
		log("        ...\n");
		log("      },\n");
		log("    }\n");
		log("\n");
		log("And <memory_details> is:\n");
		log("\n");
		log("    {\n");
		log("      \"hide_name\": <1 | 0>,\n");
		log("      \"attributes\": {\n");
		log("        <attribute_name>: <attribute_value>,\n");
		log("        ...\n");
		log("      },\n");
		log("      \"width\": <memory width>\n");
		log("      \"start_offset\": <the lowest valid memory address>\n");
		log("      \"size\": <memory size>\n");
		log("    }\n");
		log("\n");
		log("And <net_details> is:\n");
		log("\n");
		log("    {\n");
		log("      \"hide_name\": <1 | 0>,\n");
		log("      \"bits\": <bit_vector>\n");
		log("      \"offset\": <the lowest bit index in use, if non-0>\n");
		log("      \"upto\": <1 if the port bit indexing is MSB-first>\n");
		log("      \"signed\": <1 if the port is signed>\n");
		log("    }\n");
		log("\n");
		log("The \"hide_name\" fields are set to 1 when the name of this cell or net is\n");
		log("automatically created and is likely not of interest for a regular user.\n");
		log("\n");
		log("The \"port_directions\" section is only included for cells for which the\n");
		log("interface is known.\n");
		log("\n");
		log("Module and cell ports and nets can be single bit wide or vectors of multiple\n");
		log("bits. Each individual signal bit is assigned a unique integer. The <bit_vector>\n");
		log("values referenced above are vectors of this integers. Signal bits that are\n");
		log("connected to a constant driver are denoted as string \"0\", \"1\", \"x\", or\n");
		log("\"z\" instead of a number.\n");
		log("\n");
		log("Bit vectors (including integers) are written as string holding the binary");
		log("representation of the value. Strings are written as strings, with an appended");
		log("blank in cases of strings of the form /[01xz]* */.\n");
		log("\n");
		log("For example the following Verilog code:\n");
		log("\n");
		log("    module test(input x, y);\n");
		log("      (* keep *) foo #(.P(42), .Q(1337))\n");
		log("          foo_inst (.A({x, y}), .B({y, x}), .C({4'd10, {4{x}}}));\n");
		log("    endmodule\n");
		log("\n");
		log("Translates to the following JSON output:\n");
		log("\n");

		log("    {\n");
		log("      \"creator\": \"Yosys 0.9+2406 (git sha1 fb1168d8, clang 9.0.1 -fPIC -Os)\",\n");
		log("      \"modules\": {\n");
		log("        \"test\": {\n");
		log("          \"attributes\": {\n");
		log("            \"cells_not_processed\": \"00000000000000000000000000000001\",\n");
		log("            \"src\": \"test.v:1.1-4.10\"\n");
		log("          },\n");
		log("          \"ports\": {\n");
		log("            \"x\": {\n");
		log("              \"direction\": \"input\",\n");
		log("              \"bits\": [ 2 ]\n");
		log("            },\n");
		log("            \"y\": {\n");
		log("              \"direction\": \"input\",\n");
		log("              \"bits\": [ 3 ]\n");
		log("            }\n");
		log("          },\n");
		log("          \"cells\": {\n");
		log("            \"foo_inst\": {\n");
		log("              \"hide_name\": 0,\n");
		log("              \"type\": \"foo\",\n");
		log("              \"parameters\": {\n");
		log("                \"P\": \"00000000000000000000000000101010\",\n");
		log("                \"Q\": \"00000000000000000000010100111001\"\n");
		log("              },\n");
		log("              \"attributes\": {\n");
		log("                \"keep\": \"00000000000000000000000000000001\",\n");
		log("                \"module_not_derived\": \"00000000000000000000000000000001\",\n");
		log("                \"src\": \"test.v:3.1-3.55\"\n");
		log("              },\n");
		log("              \"connections\": {\n");
		log("                \"A\": [ 3, 2 ],\n");
		log("                \"B\": [ 2, 3 ],\n");
		log("                \"C\": [ 2, 2, 2, 2, \"0\", \"1\", \"0\", \"1\" ]\n");
		log("              }\n");
		log("            }\n");
		log("          },\n");
		log("          \"netnames\": {\n");
		log("            \"x\": {\n");
		log("              \"hide_name\": 0,\n");
		log("              \"bits\": [ 2 ],\n");
		log("              \"attributes\": {\n");
		log("                \"src\": \"test.v:1.19-1.20\"\n");
		log("              }\n");
		log("            },\n");
		log("            \"y\": {\n");
		log("              \"hide_name\": 0,\n");
		log("              \"bits\": [ 3 ],\n");
		log("              \"attributes\": {\n");
		log("                \"src\": \"test.v:1.22-1.23\"\n");
		log("              }\n");
		log("            }\n");
		log("          }\n");
		log("        }\n");
		log("      }\n");
		log("    }\n");
		log("\n");
		log("The models are given as And-Inverter-Graphs (AIGs) in the following form:\n");
		log("\n");
		log("    \"models\": {\n");
		log("      <model_name>: [\n");
		log("        /*   0 */ [ <node-spec> ],\n");
		log("        /*   1 */ [ <node-spec> ],\n");
		log("        /*   2 */ [ <node-spec> ],\n");
		log("        ...\n");
		log("      ],\n");
		log("      ...\n");
		log("    },\n");
		log("\n");
		log("The following node-types may be used:\n");
		log("\n");
		log("    [ \"port\", <portname>, <bitindex>, <out-list> ]\n");
		log("      - the value of the specified input port bit\n");
		log("\n");
		log("    [ \"nport\", <portname>, <bitindex>, <out-list> ]\n");
		log("      - the inverted value of the specified input port bit\n");
		log("\n");
		log("    [ \"and\", <node-index>, <node-index>, <out-list> ]\n");
		log("      - the ANDed value of the specified nodes\n");
		log("\n");
		log("    [ \"nand\", <node-index>, <node-index>, <out-list> ]\n");
		log("      - the inverted ANDed value of the specified nodes\n");
		log("\n");
		log("    [ \"true\", <out-list> ]\n");
		log("      - the constant value 1\n");
		log("\n");
		log("    [ \"false\", <out-list> ]\n");
		log("      - the constant value 0\n");
		log("\n");
		log("All nodes appear in topological order. I.e. only nodes with smaller indices\n");
		log("are referenced by \"and\" and \"nand\" nodes.\n");
		log("\n");
		log("The optional <out-list> at the end of a node specification is a list of\n");
		log("output portname and bitindex pairs, specifying the outputs driven by this node.\n");
		log("\n");
		log("For example, the following is the model for a 3-input 3-output $reduce_and cell\n");
		log("inferred by the following code:\n");
		log("\n");
		log("    module test(input [2:0] in, output [2:0] out);\n");
		log("      assign in = &out;\n");
		log("    endmodule\n");
		log("\n");
		log("    \"$reduce_and:3U:3\": [\n");
		log("      /*   0 */ [ \"port\", \"A\", 0 ],\n");
		log("      /*   1 */ [ \"port\", \"A\", 1 ],\n");
		log("      /*   2 */ [ \"and\", 0, 1 ],\n");
		log("      /*   3 */ [ \"port\", \"A\", 2 ],\n");
		log("      /*   4 */ [ \"and\", 2, 3, \"Y\", 0 ],\n");
		log("      /*   5 */ [ \"false\", \"Y\", 1, \"Y\", 2 ]\n");
		log("    ]\n");
		log("\n");
		log("Future version of Yosys might add support for additional fields in the JSON\n");
		log("format. A program processing this format must ignore all unknown fields.\n");
		log("\n");
	}
	void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
	{
		bool aig_mode = false;
		bool compat_int_mode = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-aig") {
				aig_mode = true;
				continue;
			}
			if (args[argidx] == "-compat-int") {
				compat_int_mode = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		log_header(design, "Executing JSON backend.\n");

		GmlWriter gml_writer(*f, false, aig_mode, compat_int_mode);
		gml_writer.write_design(design);
	}
} GmlBackend;

struct GmlPass : public Pass {
	GmlPass() : Pass("gml", "write design in GML format") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    gml [options] [selection]\n");
		log("\n");
		log("Write a GML netlist of all selected objects.\n");
		log("\n");
		log("    -o <filename>\n");
		log("        write to the specified file.\n");
		log("\n");
		log("    -aig\n");
		log("        also include AIG models for the different gate types\n");
		log("\n");
		log("    -compat-int\n");
		log("        emit 32-bit or smaller fully-defined parameter values directly\n");
		log("        as JSON numbers (for compatibility with old parsers)\n");
		log("\n");
		log("See 'help write_json' for a description of the JSON format used.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::string filename;
		bool aig_mode = false;
		bool compat_int_mode = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-o" && argidx+1 < args.size()) {
				filename = args[++argidx];
				continue;
			}
			if (args[argidx] == "-aig") {
				aig_mode = true;
				continue;
			}
			if (args[argidx] == "-compat-int") {
				compat_int_mode = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		std::ostream *f;
		std::stringstream buf;

		if (!filename.empty()) {
			rewrite_filename(filename);
			std::ofstream *ff = new std::ofstream;
			ff->open(filename.c_str(), std::ofstream::trunc);
			if (ff->fail()) {
				delete ff;
				log_error("Can't open file `%s' for writing: %s\n", filename.c_str(), strerror(errno));
			}
			f = ff;
		} else {
			f = &buf;
		}

		GmlWriter gml_writer(*f, true, aig_mode, compat_int_mode);
		gml_writer.write_design(design);

		if (!filename.empty()) {
			delete f;
		} else {
			log("%s", buf.str().c_str());
		}
	}
} GmlPass;

PRIVATE_NAMESPACE_END
