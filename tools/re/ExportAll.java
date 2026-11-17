// Headless export: decompiled C of every function (with entry address),
// imports, exports and defined strings, one file set per program.
// Args: <output directory>
//@category Pakon
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        File dir = new File(getScriptArgs()[0]);
        dir.mkdirs();
        String name = currentProgram.getName();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int ok = 0, failed = 0;
        try (PrintWriter c = new PrintWriter(new FileWriter(new File(dir, name + ".c")))) {
            c.println("// " + name + " - Ghidra " + getGhidraVersion() + " headless decompilation");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                c.println("\n// ==== " + f.getName() + " @ " + f.getEntryPoint());
                DecompileResults r = di.decompileFunction(f, 180, monitor);
                if (r != null && r.decompileCompleted()) {
                    c.println(r.getDecompiledFunction().getC());
                    ok++;
                } else {
                    c.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
                    failed++;
                }
            }
        }
        SymbolTable st = currentProgram.getSymbolTable();
        try (PrintWriter s = new PrintWriter(new FileWriter(new File(dir, name + ".symbols.txt")))) {
            for (Symbol sym : st.getExternalSymbols()) {
                s.println("IMPORT " + sym.getParentNamespace().getName() + "!" + sym.getName());
            }
            AddressIterator ai = st.getExternalEntryPointIterator();
            while (ai.hasNext()) {
                Address a = ai.next();
                Symbol sy = st.getPrimarySymbol(a);
                s.println("EXPORT " + a + " " + (sy == null ? "" : sy.getName()));
            }
        }
        try (PrintWriter s = new PrintWriter(new FileWriter(new File(dir, name + ".strings.txt")))) {
            DataIterator di2 = currentProgram.getListing().getDefinedData(true);
            while (di2.hasNext()) {
                Data d = di2.next();
                if (d.hasStringValue() && d.getValue() != null) {
                    s.println(d.getAddress() + " " + d.getValue().toString().replace("\n", "\\n"));
                }
            }
        }
        println("ExportAll: " + name + " functions decompiled " + ok + ", failed " + failed);
    }
}
