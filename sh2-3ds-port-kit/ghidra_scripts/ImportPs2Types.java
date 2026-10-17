// Imports the SH2 PS2 beta types (headers from tools/dwarf2h.py) into the program, one category
// per original source file: /ps2/<Folder>_<file>. Existing types in those categories are replaced.
// Args: <header dir> [file substring filter ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.program.model.data.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ImportPs2Types extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File dir = new File(args[0]);
    List<String> filters = Arrays.asList(args).subList(1, args.length);
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    File[] files = dir.listFiles((d, n) -> n.endsWith(".h"));
    Arrays.sort(files);
    int ok = 0, failed = 0, types = 0;
    for (File f : files) {
      if (!filters.isEmpty() && filters.stream().noneMatch(s -> f.getName().contains(s))) continue;
      String cat = "/ps2/" + f.getName().replaceAll("\\.(c|cpp)\\.h$", "");
      StandAloneDataTypeManager tmp = new StandAloneDataTypeManager("ps2tmp");
      int tx = tmp.startTransaction("parse");
      try {
        CParser parser = new CParser(tmp, true, null);
        parser.parse(new ByteArrayInputStream(Files.readAllBytes(f.toPath())));
      } catch (Throwable e) {
        failed++;
        println("parse failed " + f.getName() + ": " + e.getMessage().split("\n")[0]);
        tmp.endTransaction(tx, false);
        tmp.close();
        continue;
      }
      tmp.endTransaction(tx, true);
      Iterator<DataType> it = tmp.getAllDataTypes();
      while (it.hasNext()) {
        DataType dt = it.next();
        if (dt instanceof BuiltInDataType || dt instanceof Pointer || dt instanceof Array) continue;
        try {
          DataType copy = dt.clone(dtm);
          copy.setCategoryPath(new CategoryPath(cat));
          dtm.addDataType(copy, DataTypeConflictHandler.REPLACE_HANDLER);
          types++;
        } catch (Exception e) { /* skip odd types */ }
      }
      tmp.close();
      ok++;
    }
    println("imported " + types + " types from " + ok + " headers (" + failed + " failed)");
  }
}
