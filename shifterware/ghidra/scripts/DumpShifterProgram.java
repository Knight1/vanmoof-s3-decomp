// Mirror of the Ghidra script saved into ~/ghidra_scripts/DumpShifterProgram.java
// (where Ghidra discovers it). Kept here in-repo so contributors can read it
// and so a new clone can re-install it.
//
// To install on a fresh checkout:
//   cp ghidra/scripts/DumpShifterProgram.java ~/ghidra_scripts/   (Linux/macOS)
//   copy ghidra\scripts\DumpShifterProgram.java %USERPROFILE%\ghidra_scripts\  (Windows)
//
// Then run from Ghidra: Window → Script Manager → VanMoof → DumpShifterProgram
//
// Output: ghidra/exports/shifter_program.json
//
// @category VanMoof
// @author   vanmoof-s3-decomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.mem.Memory;

import java.io.File;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.List;

public class DumpShifterProgram extends GhidraScript {

    private static final String OUT =
        "C:\\Users\\Tobias\\vanmoof-s3-decomp\\ghidra\\exports\\shifter_program.json";

    @Override
    public void run() throws Exception {
        StringBuilder json = new StringBuilder();
        json.append("{\n");

        json.append("  \"program\": ").append(quote(currentProgram.getName())).append(",\n");
        json.append("  \"image_base\": \"")
            .append(currentProgram.getImageBase().toString())
            .append("\",\n");
        json.append("  \"language\": ")
            .append(quote(currentProgram.getLanguage().getLanguageID().getIdAsString()))
            .append(",\n");
        json.append("  \"compiler\": ")
            .append(quote(currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString()))
            .append(",\n");

        json.append("  \"memory_blocks\": [\n");
        Memory mem = currentProgram.getMemory();
        MemoryBlock[] blocks = mem.getBlocks();
        for (int i = 0; i < blocks.length; i++) {
            MemoryBlock b = blocks[i];
            json.append("    {");
            json.append("\"name\":").append(quote(b.getName())).append(",");
            json.append("\"start\":\"").append(b.getStart().toString()).append("\",");
            json.append("\"end\":\"").append(b.getEnd().toString()).append("\",");
            json.append("\"size\":").append(b.getSize()).append(",");
            json.append("\"r\":").append(b.isRead()).append(",");
            json.append("\"w\":").append(b.isWrite()).append(",");
            json.append("\"x\":").append(b.isExecute()).append(",");
            json.append("\"initialized\":").append(b.isInitialized());
            json.append("}").append(i + 1 < blocks.length ? "," : "").append("\n");
        }
        json.append("  ],\n");

        json.append("  \"vector_table\": [\n");
        MemoryBlock execBlock = null;
        for (MemoryBlock b : blocks) {
            if (b.isExecute() && b.isInitialized()) { execBlock = b; break; }
        }
        if (execBlock != null) {
            Address base = execBlock.getStart();
            int entries = 48;
            for (int i = 0; i < entries; i++) {
                try {
                    Address a = base.add(i * 4L);
                    int word = mem.getInt(a);
                    long u = ((long) word) & 0xFFFFFFFFL;
                    json.append("    {");
                    json.append("\"index\":").append(i).append(",");
                    json.append("\"addr\":\"").append(a.toString()).append("\",");
                    json.append("\"value\":\"0x").append(String.format("%08x", u)).append("\"");
                    json.append("}").append(i + 1 < entries ? "," : "").append("\n");
                } catch (Exception ex) {
                    break;
                }
            }
        }
        json.append("  ],\n");

        FunctionManager fm = currentProgram.getFunctionManager();
        List<Function> funcs = new ArrayList<>();
        FunctionIterator fi = fm.getFunctions(true);
        while (fi.hasNext()) funcs.add(fi.next());
        funcs.sort((a, b) -> a.getEntryPoint().compareTo(b.getEntryPoint()));

        json.append("  \"function_count\": ").append(funcs.size()).append(",\n");
        json.append("  \"functions\": [\n");
        for (int i = 0; i < funcs.size(); i++) {
            Function f = funcs.get(i);
            String name = f.getName();
            boolean autoNamed = name.startsWith("FUN_") || name.startsWith("thunk_FUN_");
            json.append("    {");
            json.append("\"addr\":\"0x")
                .append(String.format("%08x", f.getEntryPoint().getOffset()))
                .append("\",");
            json.append("\"size\":").append(f.getBody().getNumAddresses()).append(",");
            json.append("\"name\":").append(quote(name)).append(",");
            json.append("\"auto_named\":").append(autoNamed).append(",");
            json.append("\"signature\":").append(quote(f.getSignature().getPrototypeString())).append(",");
            json.append("\"thunk\":").append(f.isThunk()).append(",");
            json.append("\"external\":").append(f.isExternal());
            json.append("}").append(i + 1 < funcs.size() ? "," : "").append("\n");
        }
        json.append("  ],\n");

        DataIterator di = currentProgram.getListing().getDefinedData(true);
        List<String[]> strings = new ArrayList<>();
        while (di.hasNext()) {
            Data d = di.next();
            if (d.hasStringValue()) {
                Object v = d.getValue();
                if (v != null) {
                    strings.add(new String[] {
                        d.getAddress().toString(),
                        v.toString()
                    });
                }
            }
        }
        json.append("  \"string_count\": ").append(strings.size()).append(",\n");
        json.append("  \"strings\": [\n");
        for (int i = 0; i < strings.size(); i++) {
            String[] s = strings.get(i);
            json.append("    {\"addr\":\"").append(s[0]).append("\",\"value\":")
                .append(quote(s[1])).append("}")
                .append(i + 1 < strings.size() ? "," : "").append("\n");
        }
        json.append("  ]\n");

        json.append("}\n");

        File out = new File(OUT);
        out.getParentFile().mkdirs();
        try (FileWriter w = new FileWriter(out)) {
            w.write(json.toString());
        }

        println("DumpShifterProgram: wrote " + funcs.size() + " functions, "
                + strings.size() + " strings to " + OUT);
    }

    private static String quote(String s) {
        if (s == null) return "null";
        StringBuilder sb = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n");  break;
                case '\r': sb.append("\\r");  break;
                case '\t': sb.append("\\t");  break;
                default:
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                    else sb.append(c);
            }
        }
        sb.append("\"");
        return sb.toString();
    }
}
