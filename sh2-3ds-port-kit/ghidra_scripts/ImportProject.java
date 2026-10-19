// Puts ExportProject.java's dump back into a program: after this, a fresh import of the user's own
// exe has the same functions (exact bodies), names, tags, thunks, signatures, labels, typed globals,
// comments and bookmarks as the project it was exported from. Functions that auto-analysis made but
// the dump does not list are removed, so the function list matches exactly. Safe to run again.
// Args: <dump dir>   (sh2-3ds-port-kit/data/dc/project)
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class ImportProject extends GhidraScript {
  static final String[] KINDS = { "EOL", "PRE", "POST", "PLATE", "REPEATABLE" };

  static String unesc(String s) {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c == '\\' && i + 1 < s.length()) {
        char n = s.charAt(++i);
        sb.append(n == 't' ? '\t' : n == 'n' ? '\n' : n);
      } else sb.append(c);
    }
    return sb.toString();
  }

  List<String[]> rows(File dir, String name) throws IOException {
    List<String[]> out = new ArrayList<>();
    Path p = new File(dir, name).toPath();
    if (!Files.exists(p)) return out;
    List<String> lines = Files.readAllLines(p, StandardCharsets.UTF_8);
    for (int i = 1; i < lines.size(); i++) if (!lines.get(i).isEmpty()) out.add(lines.get(i).split("\t", -1));
    return out;
  }

  AddressSet body(String s) {
    AddressSet set = new AddressSet();
    for (String r : s.split(",")) {
      String[] lh = r.split("-");
      set.add(toAddr(lh[0]), toAddr(lh[1]));
    }
    return set;
  }

  // "A::B::name" -> name in namespace A::B (created if needed)
  void rename(Symbol s, String full, SourceType src) throws Exception {
    int k = full.lastIndexOf("::");
    Namespace ns = currentProgram.getGlobalNamespace();
    String leaf = full;
    if (k > 0) {
      ns = NamespaceUtils.createNamespaceHierarchy(full.substring(0, k), null, currentProgram, src);
      leaf = full.substring(k + 2);
    }
    if (!s.getName().equals(leaf) || !s.getParentNamespace().equals(ns) || s.getSource() != src) s.setNameAndNamespace(leaf, ns, src);
  }

  // other functions give up whatever part of b they hold
  static void clear(FunctionManager fm, Address a, AddressSet b) throws Exception {
    List<Function> hit = new ArrayList<>();
    fm.getFunctionsOverlapping(b).forEachRemaining(hit::add);
    for (Function o : hit) if (!o.getEntryPoint().equals(a)) o.setBody(new AddressSet(o.getBody()).subtract(b));
  }

  public void run() throws Exception {
    File dir = new File(getScriptArgs()[0]);
    FunctionManager fm = currentProgram.getFunctionManager();
    SymbolTable st = currentProgram.getSymbolTable();
    Listing listing = currentProgram.getListing();
    List<String[]> fns = rows(dir, "functions.tsv");

    Set<Address> want = new HashSet<>();
    for (String[] r : fns) want.add(toAddr(r[0]));
    int removed = 0, created = 0, reshaped = 0, named = 0, tagged = 0, sigs = 0, failed = 0;
    for (Function f : new ArrayList<Function>() {{ for (Function g : fm.getFunctions(true)) add(g); }}) {
      if (!want.contains(f.getEntryPoint())) { fm.removeFunction(f.getEntryPoint()); removed++; }
    }
    // bodies first (a body can only grow into space no other function holds), then names and the rest
    for (String[] r : fns) {
      Address a = toAddr(r[0]);
      AddressSet b = body(r[3]);
      Function f = fm.getFunctionAt(a);
      try {
        if (f == null) {
          if (listing.getInstructionAt(a) == null) disassemble(a);
          clear(fm, a, b);
          f = fm.createFunction(null, a, b, SourceType.DEFAULT);
          created++;
        } else if (!f.getBody().equals(b)) {
          clear(fm, a, b);
          f.setBody(b);
          reshaped++;
        }
      } catch (Exception e) {
        failed++;
        println("function " + r[0] + ": " + e.getMessage());
      }
    }
    for (String[] r : fns) {
      Function f = fm.getFunctionAt(toAddr(r[0]));
      if (f == null) continue;
      try {
        if (r[4].startsWith("0x")) {
          Function t = fm.getFunctionAt(toAddr(r[4]));
          if (t != null && (f.getThunkedFunction(false) == null || !f.getThunkedFunction(false).equals(t))) f.setThunkedFunction(t);
        }
        SourceType ns = SourceType.valueOf(r[2]);
        if (!r[1].isEmpty() && ns != SourceType.DEFAULT && !f.getName(true).equals(r[1])) { rename(f.getSymbol(), unesc(r[1]), ns); named++; }
        // analysis can name a function the dump has as FUN_* (switch cases made into functions)
        if (ns == SourceType.DEFAULT && f.getSymbol().getSource() == SourceType.ANALYSIS) {
          f.getSymbol().setNameAndNamespace("", currentProgram.getGlobalNamespace(), SourceType.DEFAULT);
          named++;
        }
        if (r[6].equals("USER_DEFINED") && !r[7].isEmpty()) {
          FunctionDefinitionDataType sig = new FunctionSignatureParser(currentProgram.getDataTypeManager(), null).parse(f.getSignature(), unesc(r[7]));
          if (new ApplyFunctionSignatureCmd(f.getEntryPoint(), sig, SourceType.USER_DEFINED).applyTo(currentProgram, monitor)) sigs++;
        }
        if (!r[5].isEmpty() && !r[5].equals("unknown") && !r[5].equals(f.getCallingConventionName()) && r[6].equals("USER_DEFINED")) f.setCallingConvention(r[5]);
        Set<String> tags = new TreeSet<>();
        if (r.length > 8 && !r[8].isEmpty()) tags.addAll(Arrays.asList(unesc(r[8]).split(",")));
        Set<String> have = new TreeSet<>();
        for (FunctionTag t : f.getTags()) have.add(t.getName());
        if (!have.equals(tags)) {
          for (String t : have) if (!tags.contains(t)) f.removeTag(t);
          for (String t : tags) if (!have.contains(t)) f.addTag(t);
          tagged++;
        }
      } catch (Exception e) {
        failed++;
        println("function " + r[0] + ": " + e.getMessage());
      }
    }

    int labels = 0;
    for (String[] r : rows(dir, "symbols.tsv")) {
      Address a = toAddr(r[0]);
      String name = unesc(r[1]);
      SourceType src = SourceType.valueOf(r[3]);
      Namespace ns = r[2].isEmpty() ? currentProgram.getGlobalNamespace()
        : NamespaceUtils.createNamespaceHierarchy(unesc(r[2]), null, currentProgram, src);
      Symbol s = st.getSymbol(name, a, ns);
      if (s == null) { s = st.createLabel(a, name, ns, src); labels++; }
      if (r[4].equals("true") && !s.isPrimary()) s.setPrimary();
    }

    int typed = 0;
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    for (String[] r : rows(dir, "data.tsv")) {
      Address a = toAddr(r[0]);
      DataType dt = dtm.getDataType(unesc(r[2]));
      if (dt == null) {
        String n = unesc(r[2]);
        if (n.equals("/pointer")) dt = PointerDataType.dataType;
        else if (n.endsWith(" *") && dtm.getDataType(n.substring(0, n.length() - 2)) != null) dt = new PointerDataType(dtm.getDataType(n.substring(0, n.length() - 2)));
      }
      Data d = listing.getDataAt(a);
      if (dt == null || (d != null && d.isDefined() && d.getDataType().isEquivalent(dt))) continue;
      try {
        listing.clearCodeUnits(a, a.add(Math.max(dt.getLength(), 1) - 1), false);
        listing.createData(a, dt);
        typed++;
      } catch (Exception e) {
        println("data " + r[0] + ": " + e.getMessage());
      }
    }

    int comments = 0;
    for (String[] r : rows(dir, "comments.tsv")) {
      Address a = toAddr(r[0]);
      int k = Arrays.asList(KINDS).indexOf(r[1]);
      String text = unesc(r[2]);
      if (!text.equals(listing.getComment(k, a))) { listing.setComment(a, k, text); comments++; }
    }

    int marks = 0;
    BookmarkManager bm = currentProgram.getBookmarkManager();
    for (String[] r : rows(dir, "bookmarks.tsv")) {
      Address a = toAddr(r[0]);
      if (bm.getBookmark(a, r[1], unesc(r[2])) == null) { bm.setBookmark(a, r[1], unesc(r[2]), unesc(r[3])); marks++; }
    }

    println("functions: " + created + " created, " + reshaped + " reshaped, " + removed + " removed, " + named + " renamed, "
      + tagged + " retagged, " + sigs + " signatures, " + failed + " failed; labels " + labels + ", typed globals " + typed
      + ", comments " + comments + ", bookmarks " + marks);
  }
}
