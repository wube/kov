"""Assigns every macro from macro_survey.json to one of the hand-made categories of the Kov macro
survey and sums definitions and use sites per category. Prints leftovers so the rules can be
refined. Rules are by name first, then by what the body does."""
import json, re, collections, os

defs = json.load(open(os.path.join(os.getcwd(), "macro_survey.json")))

CATS = [
 ("A assert / abort / log with source location",
  r"^(assert|release_assert|blueprintAssert|LOG|LOG_[A-Z_]+|FLUSH_LOG|WHEN_ASSERT|ASSERT_RENDER_THREAD|OPENGL_INTERFACE_CHECK_ERRORS|PRINT_SIZE|EXECUTE_FOR_LOG_DEBUG|debug_friendly_vector_for|_debug_friendly_vector_for|MIP_CHECK|CHECK_MAX|CHECK_MIN|CHECK_FLOAT_CAST[A-Z_]*|CHECK_GHOST_FLAG|CHECK_VALID_TILE_SPRITE|TERRITORY_SWITCH)$"),
 ("B test-suite helpers",
  r"^(CHECK_[A-Z_0-9]+|SETUP_[A-Z_]+|TEMPORARILY_[A-Z_]+|SAVE_FILE|DEALOC|TEST_GENERATOR|STR|NINE_CHUNKS|ASTEROID_COLLECTOR_ENTITY_NAME|CHECK_MOVE_RESULT|CHECK_SORT)$"),
 ("C member names as strings (reflection by hand)",
  r"^(ADD_VALUE|ADD_ENUM|SKIP_VALUE|MAKE_FOR_TABLE[A-Z_]*|MAKE_ENUM_FOR_TABLE[A-Z_]*|SAVE_UTILITY_CONSTANT|LOAD_UTILITY_CONSTANT[A-Z_]*|DBG_VAR|DBG_MULTI_VAR|VARIABLE_NAME|EQUALS_COMPARE|MAKE_GETTER|MAKE_SETTER|COPY_FROM_MAP_VIEW_SETTINGS|FOR_EACH_FIELD|MAKE_VERIFIER)$"),
 ("D lists of types/values with code generated per entry",
  r"^(CALL_ON_EVERY_[A-Z_0-9]+|FOR_EACH_SUPPORTED_TYPE|REGISTER_LOADER|CHECK_TYPE|INSERT_[A-Z_]+|ID_CONNECTOR_MAKE|EXPLICIT_[A-Z_]+|GENERATE_[A-Z_]+|CLONE|PARSE|HANDLE_CASE|CALL_EMPLACE|CREATE_[A-Z_]+|MAKE_NAMED_CONSTRUCTOR|DEFINE_[A-Z_]+|ADD_ACTION|ADD_CUSTOM_ACTION|ADD_EDITOR_ACTION|ADD_QUICKBAR_[A-Z_]+|SIMPLE_BUILDER|IMPL_STATIC_FUNC_PTR|GET_INSTANCE_FUNC_ADDR|ADD_STATIC_FUNC_PTR|CLEAR_INSTANCE_FUNC_ADDR|REGISTER_[A-Z_]+|[A-Z_]*_ID_TYPE[A-Z_]*|STYLE_[A-Z_]+|[A-Z_]+_STYLE|MIGRATION_[A-Z_]+|[A-Z_]*MIGRATION[A-Z_]*|CONCAT_STRING|ACTION_[A-Z_]+|INPUT_ACTION[A-Z_]*|EDITOR_ACTION[A-Z_]*|[A-Z_]+_ACTION_[A-Z_]+|BUILD_[A-Z_]+_FUNCTION|LUA_[A-Z_]+|SCRIPT_[A-Z_]+)$"),
 ("E enum and bit-field definition machinery",
  r"^(ENUM|ENUM_IMPL|ENUM_IMPL_NOSTRING|_ENUM_[A-Z_0-9]+|BITFIELD|_BITFIELD_[A-Z_0-9]+|ENUM_[A-Z_]+)$"),
 ("F scoped objects needing unique names, local macro protocols",
  r"^(SAVING_TAG|SAVING_STATISTICS|SAVING_[A-Z_0-9]+|INIT_SPRITE_DRAWING|PUSH_TO_[A-Z_]+)$"),
 ("G compiler modifiers, pragmas, warnings",
  r"^(VCALL|NO_INLINE|NOINLINE|FORCE_INLINE|FORCEINLINE|ALWAYS_INLINE|FORMAT_ATTR|TAG_FORMAT_ATTR|FORMAT_STRING|DEV_TOOL_VARIABLE|BEGIN_SUPPRESS_CONVERSIONS|END_SUPPRESS_CONVERSIONS|BEGIN_FORCE_OPTIMIZE|END_FORCE_OPTIMIZE|PADDING_WARNING|IMGUI_API|IMGUI_IMPL_API|[A-Z_]*_API|[A-Z_]*EXPORT[A-Z_]*|[A-Z_]*INLINE[A-Z_]*|UNUSED[A-Z_]*|LIKELY|UNLIKELY|DEPRECATED[A-Z_]*|PACKED|ALIGNED?[A-Z_0-9]*|RESTRICT|THREAD_LOCAL|NORETURN|[A-Z_]*WARNING[A-Z_]*|[A-Z_]*PRAGMA[A-Z_]*|SUPPRESS_[A-Z_]+|[A-Z_]*OPTIMIZE[A-Z_]*)$"),
 ("H configuration flags and platform conditionals",
  r"^(DEV_ONLY_DEBUG_TOOL|HAS_SSE2|HAS_[A-Z_0-9]+|ENABLE_[A-Z_]+|[A-Z_]+_ENABLED|USE_[A-Z_]+|DEBUG_[A-Z_]+|FOR_DEVELOPER|LOG_DEBUG_ACTIVE|CRC_CHECK_SAVE_LOAD|DO_DELETE_FILES|DELETE_FILES_IN_MOD|[A-Z_]*DEBUG[A-Z_]*|NINTENDO[A-Z_0-9]*|STEAM[A-Z_]*|WIN32|LINUX|MAC_OSX|DEMO|FACTORIO_[A-Z_]+|HEADLESS|SPACE_AGE|SUPER_HEAVY|ALL_IN_ONE|TEST_[A-Z_]+|[A-Z_]+_TEST|WITH_[A-Z_]+|NO_[A-Z_]+|DISABLE_[A-Z_]+|TRACY[A-Z_]*|GPU_[A-Z_]+|[A-Z_]+_PROFILER|UNITTEST[A-Z_]*|COMMON_HPP|USE_COMMON_HPP)$"),
 ("I constants and string tables",
  r"^(UTF8_[A-Z_0-9]+|M_[A-Z_0-9]+|INSET_[A-Z_]+|C565_[A-Z_0-9]+|BUILD_MODE|RZRESULT_[A-Z_0-9]+|[A-Z_]+_KEY_NAME|DestBlockWidth|[A-Z_]*VERSION[A-Z_0-9]*|[A-Z_]*_SIZE|[A-Z_]*_LENGTH|[A-Z_]*_COUNT|[A-Z_]*_MASK|[A-Z_]*_SHIFT|[A-Z_]*_BITS|[A-Z_]*MAX[A-Z_]*|[A-Z_]*MIN_[A-Z_]*|[A-Z_]*_NAME|[A-Z_]*_PATH|[A-Z_]*_URL|[A-Z_]*_PORT|[A-Z_]*_ID|LOGI_[A-Z_0-9]+|[A-Z_]*_COLOR|[A-Z_]+_STRING|[A-Z_]*_CODE|ApplicationVersion[A-Za-z]*|[A-Z_]*_NUMBER|[A-Z_]*_FLAG)$"),
 ("J aliases and library workarounds",
  r"^(min|max|fileno|fseeko64|ftello64|vk[A-Za-z]+|cl[A-Z][A-Za-z]+|load_m128i|store_m128i|toCStr|ARG_STRING_TYPE|ARG_EQUAL|[a-z_0-9]+)$"),
]
CAT_RES = [(name, re.compile(rx)) for name, rx in CATS]

def by_body(d):
    body, params = d["body"], d["params"]
    if params is None and body == "":
        return "H configuration flags and platform conditionals"
    if re.fullmatch(r"[-+*/%()<>|&~^ 0-9a-fA-FxXuUlL.'eE]+", body) or re.fullmatch(r'"(?:[^"\\]|\\.)*"( ?//.*)?', body) or re.fullmatch(r"[-+(]*\s*(0[xX][0-9a-fA-F']+|[0-9][0-9']*\.?[0-9]*([eE][-+]?[0-9]+)?)[uUlLfF]*\s*[)]*( ?//.*)?", body):
        return "I constants and string tables"
    if re.match(r"^(__forceinline|__declspec\(|__attribute__\(\(|inline$|\[\[|__builtin_expect|__restrict|__pragma\(|_Pragma\()", body):
        return "G compiler modifiers, pragmas, warnings"
    if "__FILE__" in body or "__LINE__" in body or "__func" in body or "__FUNCTION__" in body:
        return "A assert / abort / log with source location"
    if params and "MACRO" in params:
        return "D lists of types/values with code generated per entry"
    if params is None and re.fullmatch(r"[A-Za-z_][\w:<>, ]*", body):
        return "J aliases and library workarounds"
    if "##" in body or re.search(r"(^|[^#])#[A-Za-z_]", body):
        return "C member names as strings (reflection by hand)"
    return "K inline code snippets (should be functions or lambdas)"

def category(d):
    for name, rx in CAT_RES:
        if rx.match(d["name"]):
            return name
    if "\\tests\\" in d["file"] or d["file"].startswith("tests"):
        return "B test-suite helpers"
    return by_body(d)

cats = collections.defaultdict(list)
seen = set()
for d in sorted(defs, key=lambda d: -d["uses"]):
    c = category(d)
    d["cat"] = c
    cats[c].append(d)

order = sorted(cats, key=lambda c: -sum(d["uses"] for d in cats[c] if (d["name"] not in seen and not seen.add(d["name"]))))
seen = set()
print(f"{'use sites':>9} {'names':>5} {'defs':>5}  category")
total_uses = total_names = 0
for c in order:
    names = {}
    for d in cats[c]:
        names.setdefault(d["name"], d["uses"])
    uses = sum(names.values())
    total_uses += uses; total_names += len(names)
    print(f"{uses:9d} {len(names):5d} {len(cats[c]):5d}  {c}")
print(f"{total_uses:9d} {total_names:5d} {len(defs):5d}  total")
print()
for c in order:
    print("==", c)
    names = {}
    for d in cats[c]:
        if d["name"] not in names:
            names[d["name"]] = d
    for name, d in sorted(names.items(), key=lambda kv: -kv[1]["uses"])[:14]:
        print(f"   {d['uses']:5d}  {name}{d['params'] or ''}  {d['file']}:{d['line']}  = {d['body'][:90]}")
    rest = len(names) - 14
    if rest > 0:
        print(f"   ... {rest} more: " + ", ".join(sorted(n for n in list(names)[14:]))[:400])
    print()
