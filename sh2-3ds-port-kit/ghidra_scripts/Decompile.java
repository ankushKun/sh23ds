// Decompiles functions by address, name or any address inside them, into one C file. Used by
// tools/ghidra/dec.sh. Each function is preceded by its name, entry, size, tags and callers.
// Args: <outfile> <addr|name> ...
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Decompile extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    FunctionManager fm = currentProgram.getFunctionManager();
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    try (PrintWriter pw = new PrintWriter(a[0], "UTF-8")) {
      for (int i = 1; i < a.length; i++) {
        List<Function> fs = new ArrayList<>();
        if (a[i].matches("(0x)?[0-9a-fA-F]{6,8}")) {
          Function f = fm.getFunctionContaining(toAddr(a[i]));
          if (f != null) fs.add(f);
        } else {
          for (Symbol s : currentProgram.getSymbolTable().getGlobalSymbols(a[i])) {
            Function f = fm.getFunctionAt(s.getAddress());
            if (f != null) fs.add(f);
          }
        }
        if (fs.isEmpty()) { pw.println("// ===== " + a[i] + ": no function"); println(a[i] + ": no function"); continue; }
        for (Function f : fs) {
          TreeSet<String> tags = new TreeSet<>(), callers = new TreeSet<>();
          for (FunctionTag t : f.getTags()) tags.add(t.getName());
          for (Function c : f.getCallingFunctions(monitor)) callers.add(c.getName() + "@" + c.getEntryPoint());
          pw.println("// ===== " + f.getName(true) + " @ 0x" + f.getEntryPoint() + ", " + f.getBody().getNumAddresses() + " bytes"
            + (tags.isEmpty() ? "" : ", " + String.join(" ", tags)));
          pw.println("// callers: " + (callers.isEmpty() ? "none found" : String.join(" ", callers)));
          DecompileResults r = di.decompileFunction(f, 120, monitor);
          pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
      }
    }
    di.dispose();
  }
}
