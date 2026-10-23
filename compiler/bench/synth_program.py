"""Generates a large synthetic Kov program to measure the backend at scale.

Usage: python synth_program.py <modules> <out-dir>

Each module is one file with 10 classes; every class has fields, a constructor, plain methods
that do arithmetic over the fields and call methods of classes in other modules, one virtual
method overriding a shared base, and a function using Vector<int>. The shape follows Factorio's
averages (about 9 function bodies per 1,000 lines of declarations... roughly 170 lines per file,
15-line methods), so N modules ~ N * 300 lines. main() touches one object per class so nothing
is dead code. Everything stays inside the Kov v0 subset codegen supports today.
"""
import os, sys

modules = int(sys.argv[1]) if len(sys.argv) > 1 else 10
out = sys.argv[2] if len(sys.argv) > 2 else "synth"
os.makedirs(out, exist_ok=True)
CLASSES = 10

def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

common = """// Shared base of every generated class.
struct Shape
{
  int id = 0;
  virtual ~Shape() {}
  virtual int area() const { return 0; }
  virtual int weight() const { return this->id; }
};

int mix(int a, int b) { return (a * 31 + b) ^ (a >> 3); }
"""
write(os.path.join(out, "Common.kov"), common)

for m in range(modules):
    lines = ["// Generated module %d." % m, ""]
    for c in range(CLASSES):
        name = "C%d_%d" % (m, c)
        other = "C%d_%d" % ((m + 1) % modules, (c + 3) % CLASSES)
        lines.append("struct %s : Shape" % name)
        lines.append("{")
        lines.append("  int a; int b; int c;")
        lines.append("  Vector<int> items;")
        lines.append("  %s(int seed) : a(seed), b(seed * 7 + %d), c(seed - %d) { this->id = seed; }" % (name, c, m))
        lines.append("  int area() const override { return this->a * this->b + this->c; }")
        lines.append("  int weight() const override { return mix(this->a, this->b) + Shape::weight(); }")
        for k in range(5):
            lines.append("  int step%d(int x) const" % k)
            lines.append("  {")
            lines.append("    int total = this->a + x * %d;" % (k + 1))
            lines.append("    for (int i = 0; i < 4; ++i)")
            lines.append("      total = mix(total, this->b + i * %d);" % (k + 2))
            lines.append("    if (total %% 7 == %d)" % k)
            lines.append("      total += this->c;")
            lines.append("    else")
            lines.append("      total -= this->c >> 1;")
            lines.append("    return total;")
            lines.append("  }")
        lines.append("  int run(int rounds)")
        lines.append("  {")
        lines.append("    this->items.clear();")
        lines.append("    for (int r = 0; r < rounds; ++r)")
        lines.append("      this->items.push_back(this->step%d(r) + this->step%d(r * 2));" % (c % 5, (c + 1) % 5))
        lines.append("    int sum = 0;")
        lines.append("    for (int v : this->items)")
        lines.append("      sum = mix(sum, v);")
        lines.append("    %s neighbour(this->a + 1);" % other)
        lines.append("    return sum + neighbour.area() + neighbour.step%d(sum);" % ((c + 2) % 5))
        lines.append("  }")
        lines.append("};")
        lines.append("")
    lines.append("int runModule%d(int seed)" % m)
    lines.append("{")
    lines.append("  int total = 0;")
    for c in range(CLASSES):
        lines.append("  { C%d_%d object(seed + %d); total = mix(total, object.run(8) + object.weight()); }" % (m, c, c))
    lines.append("  return total;")
    lines.append("}")
    lines.append("")
    write(os.path.join(out, "Module%d.kov" % m), "\n".join(lines))

main = ["int main()", "{", "  int total = 0;"]
for m in range(modules):
    main.append("  total = mix(total, runModule%d(%d));" % (m, m))
main.append('  println("{}", total);')
main.append("  return 0;")
main.append("}")
main.append("")
write(os.path.join(out, "Main.kov"), "\n".join(main))
total_lines = sum(sum(1 for _ in open(os.path.join(out, f), encoding="utf-8")) for f in os.listdir(out) if f.endswith(".kov"))
print("modules %d classes %d lines %d" % (modules, modules * CLASSES, total_lines))
