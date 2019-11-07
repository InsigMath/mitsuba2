#include <cctype>
#include <fstream>
#include <set>
#include <unordered_map>

#include <mitsuba/core/class.h>
#include <mitsuba/core/filesystem.h>
#include <mitsuba/core/fresolver.h>
#include <mitsuba/core/logger.h>
#include <mitsuba/core/math.h>
#include <mitsuba/core/object.h>
#include <mitsuba/core/plugin.h>
#include <mitsuba/core/profiler.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/spectrum.h>
#include <mitsuba/core/string.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/core/vector.h>
#include <mitsuba/core/xml.h>
#include <mitsuba/render/spectrum.h>
#include <pugixml.hpp>
#include <tbb/tbb.h>

/// Linux <sys/sysmacros.h> defines these as macros .. :(
#if defined(major)
#  undef major
#endif

#if defined(minor)
#  undef minor
#endif

NAMESPACE_BEGIN(mitsuba)
NAMESPACE_BEGIN(xml)

/* Set of supported XML tags */
enum ETag {
    EBoolean, EInteger, EFloat, EString, EPoint, EVector, ESpectrum, ERGB,
    EColor, ETransform, ETranslate, EMatrix, ERotate, EScale, ELookAt, EObject,
    ENamedReference, EInclude, EAlias, EDefault, EInvalid
};

struct Version {
    unsigned int major, minor, patch;

    Version() = default;

    Version(int major, int minor, int patch)
        : major(major), minor(minor), patch(patch) { }

    Version(const char *value) {
        auto list = string::tokenize(value, " .");
        if (list.size() != 3)
            Throw("Version number must consist of three period-separated parts!");
        major = std::stoul(list[0]);
        minor = std::stoul(list[1]);
        patch = std::stoul(list[2]);
    }

    bool operator==(const Version &v) const {
        return std::tie(major, minor, patch) ==
               std::tie(v.major, v.minor, v.patch);
    }

    bool operator!=(const Version &v) const {
        return std::tie(major, minor, patch) !=
               std::tie(v.major, v.minor, v.patch);
    }

    bool operator<(const Version &v) const {
        return std::tie(major, minor, patch) <
               std::tie(v.major, v.minor, v.patch);
    }

    friend std::ostream &operator<<(std::ostream &os, const Version &v) {
        os << v.major << "." << v.minor << "." << v.patch;
        return os;
    }
};

NAMESPACE_BEGIN(detail)

/// Throws if non-whitespace characters are found after the given index.
void check_whitespace_only(const std::string &s, size_t offset) {
    for (size_t i = offset; i < s.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(s[i])))
            Throw("Invalid trailing characters while parsing float from string"
                  " \"%s\"", s);
    }
}

inline Float stof(const std::string &s) {
    size_t offset = 0;
    Float result = std::stof(s, &offset);
    check_whitespace_only(s, offset);
    return result;
}

inline int64_t stoll(const std::string &s) {
    size_t offset = 0;
    int64_t result = std::stoll(s, &offset);
    check_whitespace_only(s, offset);
    return result;
}


static std::unordered_map<std::string, ETag> *tags = nullptr;
static std::unordered_map<std::string /* e.g. bsdf.scalar-rgb */,
                          const Class *> *tag_class = nullptr;

// Called by Class::Class()
void register_class(const Class *class_) {
    if (!tags) {
        tags = new std::unordered_map<std::string, ETag>();
        tag_class = new std::unordered_map<std::string, const Class *>();

        /* Create an initial mapping of tag names to IDs */
        (*tags)["boolean"]    = EBoolean;
        (*tags)["integer"]    = EInteger;
        (*tags)["float"]      = EFloat;
        (*tags)["string"]     = EString;
        (*tags)["point"]      = EPoint;
        (*tags)["vector"]     = EVector;
        (*tags)["transform"]  = ETransform;
        (*tags)["translate"]  = ETranslate;
        (*tags)["matrix"]     = EMatrix;
        (*tags)["rotate"]     = ERotate;
        (*tags)["scale"]      = EScale;
        (*tags)["lookat"]     = ELookAt;
        (*tags)["ref"]        = ENamedReference;
        (*tags)["spectrum"]   = ESpectrum;
        (*tags)["rgb"]        = ERGB;
        (*tags)["color"]      = EColor;
        (*tags)["include"]    = EInclude;
        (*tags)["alias"]      = EAlias;
        (*tags)["default"]    = EDefault;
    }

    /* Register the new class as an object tag */
    auto tag_name = class_->alias();
    auto tag_key = Class::construct_key(tag_name, class_->variant());
    auto it = tags->find(tag_name);
    if (it == tags->end()) {
        (*tags)[tag_name] = EObject;
        (*tag_class)[tag_key] = class_;
    } else if (it->second != EObject) {
        (*tag_class)[tag_key] = class_;
    }

    if (tag_name == "spectrum") {
        // A texture is a kind of ContinuousSpectrum
        (*tags)["texture"] = EObject;
        (*tag_class)[Class::construct_key("texture", class_->variant())] = class_;
    }
}

// Called by Class::static_shutdown()
void cleanup() {
    delete tags;
    delete tag_class;
}

/// Helper function: map a position offset in bytes to a more readable line/column value
static std::string string_offset(const std::string &string, ptrdiff_t pos) {
    std::istringstream is(string);
    char buffer[1024];
    int line = 0, line_start = 0, offset = 0;
    while (is.good()) {
        is.read(buffer, sizeof(buffer));
        for (int i = 0; i < is.gcount(); ++i) {
            if (buffer[i] == '\n') {
                if (offset + i >= pos)
                    return tfm::format("line %i, col %i", line + 1, pos - line_start);
                ++line;
                line_start = offset + i;
            }
        }
        offset += (int) is.gcount();
    }
    return "byte offset " + std::to_string(pos);
}

/// Helper function: map a position offset in bytes to a more readable line/column value
static std::string file_offset(const fs::path &filename, ptrdiff_t pos) {
    std::fstream is(filename.native());
    char buffer[1024];
    int line = 0, line_start = 0, offset = 0;
    while (is.good()) {
        is.read(buffer, sizeof(buffer));
        for (int i = 0; i < is.gcount(); ++i) {
            if (buffer[i] == '\n') {
                if (offset + i >= pos)
                    return tfm::format("line %i, col %i", line + 1, pos - line_start);
                ++line;
                line_start = offset + i;
            }
        }
        offset += (int) is.gcount();
    }
    return "byte offset " + std::to_string(pos);
}

struct XMLSource {
    std::string id;
    const pugi::xml_document &doc;
    std::function<std::string(ptrdiff_t)> offset;
    size_t depth = 0;
    bool modified = false;

    template <typename... Args>
    [[noreturn]]
    void throw_error(const pugi::xml_node &n, const std::string &msg_, Args&&... args) {
        std::string msg = "Error while loading \"%s\" (at %s): " + msg_ + ".";
        Throw(msg.c_str(), id, offset(n.offset_debug()), args...);
    }
};

struct XMLObject {
    Properties props;
    const Class *class_ = nullptr;
    std::string src_id;
    std::string alias;
    std::function<std::string(ptrdiff_t)> offset;
    size_t location = 0;
    ref<Object> object;
    tbb::spin_mutex mutex;
};

struct XMLParseContext {
    std::unordered_map<std::string, XMLObject> instances;
    Transform4f transform;
    size_t id_counter = 0;
    std::string variant;
};

/// Helper function to check if attributes are fully specified
static void check_attributes(XMLSource &src, const pugi::xml_node &node,
                             std::set<std::string> &&attrs, bool expect_all = true) {
    bool found_one = false;
    for (auto attr : node.attributes()) {
        auto it = attrs.find(attr.name());
        if (it == attrs.end())
            src.throw_error(node, "unexpected attribute \"%s\" in element \"%s\"", attr.name(), node.name());
        attrs.erase(it);
        found_one = true;
    }
    if (!attrs.empty() && (!found_one || expect_all))
        src.throw_error(node, "missing attribute \"%s\" in element \"%s\"", *attrs.begin(), node.name());
}


/// Helper function to split the 'value' attribute into X/Y/Z components
void expand_value_to_xyz(XMLSource &src, pugi::xml_node &node) {
    if (node.attribute("value")) {
        auto list = string::tokenize(node.attribute("value").value());
        if (node.attribute("x") || node.attribute("y") || node.attribute("z"))
            src.throw_error(node, "can't mix and match \"value\" and \"x\"/\"y\"/\"z\" attributes");
        if (list.size() == 1) {
            node.append_attribute("x") = list[0].c_str();
            node.append_attribute("y") = list[0].c_str();
            node.append_attribute("z") = list[0].c_str();
        } else if (list.size() == 3) {
            node.append_attribute("x") = list[0].c_str();
            node.append_attribute("y") = list[1].c_str();
            node.append_attribute("z") = list[2].c_str();
        } else {
            src.throw_error(node, "\"value\" attribute must have exactly 1 or 3 elements");
        }
        node.remove_attribute("value");
    }
}

Vector3f parse_named_vector(XMLSource &src, pugi::xml_node &node, const std::string &attr_name) {
    auto vec_str = node.attribute(attr_name.c_str()).value();
    auto list = string::tokenize(vec_str);
    if (list.size() != 3)
        src.throw_error(node, "\"%s\" attribute must have exactly 3 elements", attr_name);
    try {
        return Vector3f(detail::stof(list[0]),
                        detail::stof(list[1]),
                        detail::stof(list[2]));
    } catch (...) {
        src.throw_error(node, "could not parse floating point values in \"%s\"", vec_str);
    }
}

Vector3f parse_vector(XMLSource &src, pugi::xml_node &node, Float def_val = 0.f) {
    std::string value;
    try {
        Float x = def_val, y = def_val, z = def_val;
        value = node.attribute("x").value();
        if (!value.empty()) x = detail::stof(value);
        value = node.attribute("y").value();
        if (!value.empty()) y = detail::stof(value);
        value = node.attribute("z").value();
        if (!value.empty()) z = detail::stof(value);
        return Vector3f(x, y, z);
    } catch (...) {
        src.throw_error(node, "could not parse floating point value \"%s\"", value);
    }
}

void upgrade_tree(XMLSource &src, pugi::xml_node &node, const Version &version) {
    if (version == Version(MTS_VERSION_MAJOR, MTS_VERSION_MINOR, MTS_VERSION_PATCH))
        return;

    Log(Info, "\"%s\": upgrading document from v%s to v%s ..", src.id, version,
        Version(MTS_VERSION));

    if (version < Version(2, 0, 0)) {
        /* Upgrade all attribute names from camelCase to underscore_case */
        for (pugi::xpath_node result: node.select_nodes("//@name")) {
            pugi::xml_attribute name_attrib = result.attribute();
            std::string name = name_attrib.value();
            for (size_t i = 0; i < name.length() - 1; ++i) {
                if (std::islower(name[i]) && std::isupper(name[i + 1])) {
                    name = name.substr(0, i + 1) + std::string("_") + name.substr(i + 1);
                    i += 2;
                    while (i < name.length() && std::isupper(name[i])) {
                        name[i] = std::tolower(name[i]);
                        ++i;
                    }
                }
            }
            name_attrib.set_value(name.c_str());
        }
        for (pugi::xpath_node result: node.select_nodes("//lookAt"))
            result.node().set_name("lookat");

        /* Update 'uoffset', 'voffset', 'uscale', 'vscale' to transform block */
        for (pugi::xpath_node result : node.select_nodes(
                 "//node()[float[@name='uoffset' or @name='voffset' or "
                 "@name='uscale' or @name='vscale']]")) {
            pugi::xml_node n = result.node();
            pugi::xml_node uoffset = n.select_node("float[@name='uoffset']").node();
            pugi::xml_node voffset = n.select_node("float[@name='voffset']").node();
            pugi::xml_node uscale  = n.select_node("float[@name='uscale']").node();
            pugi::xml_node vscale  = n.select_node("float[@name='vscale']").node();

            Vector2f offset(0.f), scale(1.f);
            if (uoffset) {
                offset.x() = stof(uoffset.attribute("value").value());
                n.remove_child(uoffset);
            }
            if (voffset) {
                offset.y() = stof(voffset.attribute("value").value());
                n.remove_child(voffset);
            }
            if (uscale) {
                scale.x() = stof(uscale.attribute("value").value());
                n.remove_child(uscale);
            }
            if (vscale) {
                scale.y() = stof(vscale.attribute("value").value());
                n.remove_child(vscale);
            }

            pugi::xml_node trafo = n.append_child("transform");
            trafo.append_attribute("name") = "to_uv";

            if (offset != Vector2f(0.f)) {
                pugi::xml_node element = trafo.append_child("translate");
                element.append_attribute("x") = std::to_string(offset.x()).c_str();
                element.append_attribute("y") = std::to_string(offset.y()).c_str();
            }

            if (scale != Vector2f(1.f)) {
                pugi::xml_node element = trafo.append_child("scale");
                element.append_attribute("x") = std::to_string(scale.x()).c_str();
                element.append_attribute("y") = std::to_string(scale.y()).c_str();
            }
        }
    }

    src.modified = true;
}

static std::pair<std::string, std::string>
parse_xml(XMLSource &src, XMLParseContext &ctx, pugi::xml_node &node,
         ETag parent_tag, Properties &props, ParameterList &param,
         size_t &arg_counter, int depth, bool within_emitter = false) {
    try {
        if (!param.empty()) {
            for (auto attr: node.attributes()) {
                std::string value = attr.value();
                if (value.find('$') == std::string::npos)
                    continue;
                for (const auto &kv : param)
                    string::replace_inplace(value, "$" + kv.first, kv.second);
                attr.set_value(value.c_str());
            }
        }

        /* Skip over comments */
        if (node.type() == pugi::node_comment || node.type() == pugi::node_declaration)
            return std::make_pair("", "");

        if (node.type() != pugi::node_element)
            src.throw_error(node, "unexpected content");

        /* Look up the name of the current element */
        auto it = tags->find(node.name());
        if (it == tags->end())
            src.throw_error(node, "unexpected tag \"%s\"", node.name());

        ETag tag = it->second;

        if (node.attribute("type") && tag != EObject
            && tag_class->find(Class::construct_key(node.name(), ctx.variant)) != tag_class->end())
            tag = EObject;

        /* Perform some safety checks to make sure that the XML tree really makes sense */
        bool has_parent              = parent_tag != EInvalid;
        bool parent_is_object        = has_parent && parent_tag == EObject;
        bool current_is_object       = tag == EObject;
        bool parent_is_transform     = parent_tag == ETransform;
        bool current_is_transform_op = tag == ETranslate || tag == ERotate ||
                                       tag == EScale || tag == ELookAt ||
                                       tag == EMatrix;

        if (!has_parent && !current_is_object)
            src.throw_error(node, "root element \"%s\" must be an object", node.name());

        if (parent_is_transform != current_is_transform_op) {
            if (parent_is_transform)
                src.throw_error(node, "transform nodes can only contain transform operations");
            else
                src.throw_error(node, "transform operations can only occur in a transform node");
        }

        if (has_parent && !parent_is_object && !(parent_is_transform && current_is_transform_op))
            src.throw_error(node, "node \"%s\" cannot occur as child of a property", node.name());

        auto version_attr = node.attribute("version");

        if (depth == 0 && !version_attr)
            src.throw_error(node, "missing version attribute in root element \"%s\"", node.name());

        if (version_attr) {
            Version version;
            try {
                version = version_attr.value();
            } catch (const std::exception &) {
                src.throw_error(node, "could not parse version number \"%s\"", version_attr.value());
            }
            upgrade_tree(src, node, version);
            node.remove_attribute(version_attr);
        }

        if (std::string(node.name()) == "scene") {
            node.append_attribute("type") = "scene";
        } else if (tag == ETransform) {
            ctx.transform = Transform4f();
        }

        if (node.attribute("name")) {
            auto name = node.attribute("name").value();
            if (string::starts_with(name, "_"))
                src.throw_error(
                    node, "invalid parameter name \"%s\" in element \"%s\": leading "
                          "underscores are reserved for internal identifiers.",
                    name, node.name());
        } else if (current_is_object || tag == ENamedReference) {
            node.append_attribute("name") = tfm::format("_arg_%i", arg_counter++).c_str();
        }

        if (node.attribute("id")) {
            auto id = node.attribute("id").value();
            if (string::starts_with(id, "_"))
                src.throw_error(
                    node, "invalid id \"%s\" in element \"%s\": leading "
                          "underscores are reserved for internal identifiers.",
                    id, node.name());
        } else if (current_is_object) {
            node.append_attribute("id") = tfm::format("_unnamed_%i", ctx.id_counter++).c_str();
        }

        switch (tag) {
            case EObject: {
                    check_attributes(src, node, { "type", "id", "name" });
                    std::string id        = node.attribute("id").value(),
                                name      = node.attribute("name").value(),
                                type      = node.attribute("type").value(),
                                node_name = node.name();

                    Properties props_nested;
                    props_nested.set_id(id);

                    auto it_inst = ctx.instances.find(id);
                    if (it_inst != ctx.instances.end())
                        src.throw_error(node, "\"%s\" has duplicate id \"%s\" (previous was at %s)",
                            node_name, id, src.offset(it_inst->second.location));

                    auto it2 = tag_class->find(Class::construct_key(node_name, ctx.variant));
                    if (it2 == tag_class->end())
                        src.throw_error(node, "could not retrieve class object for "
                                       "tag \"%s\"", node_name);

                    size_t arg_counter_nested = 0;
                    for (pugi::xml_node &ch: node.children()) {
                        auto [arg_name, nested_id] =
                            parse_xml(src, ctx, ch, tag, props_nested, param,
                                      arg_counter_nested, depth + 1,
                                      node_name == "emitter");
                        if (!nested_id.empty())
                            props_nested.set_named_reference(arg_name, nested_id);
                    }

                    auto &inst = ctx.instances[id];
                    inst.props = props_nested;
                    inst.class_ = it2->second;
                    inst.offset = src.offset;
                    inst.src_id = src.id;
                    inst.location = node.offset_debug();
                    return std::make_pair(name, id);
                }
                break;

            case ENamedReference: {
                    check_attributes(src, node, { "name", "id" });
                    auto id = node.attribute("id").value();
                    auto name = node.attribute("name").value();
                    return std::make_pair(name, id);
                }
                break;

            case EAlias: {
                    check_attributes(src, node, { "id", "as" });
                    auto alias_src = node.attribute("id").value();
                    auto alias_dst = node.attribute("as").value();
                    auto it_alias_dst = ctx.instances.find(alias_dst);
                    if (it_alias_dst != ctx.instances.end())
                        src.throw_error(node, "\"%s\" has duplicate id \"%s\" (previous was at %s)",
                            node.name(), alias_dst, src.offset(it_alias_dst->second.location));
                    auto it_alias_src = ctx.instances.find(alias_src);
                    if (it_alias_src == ctx.instances.end())
                        src.throw_error(node, "referenced id \"%s\" not found", alias_src);

                    auto &inst = ctx.instances[alias_dst];
                    inst.alias = alias_src;
                    inst.offset = src.offset;
                    inst.src_id = src.id;
                    inst.location = node.offset_debug();

                    return std::make_pair("", "");
                }
                break;

            case EDefault: {
                    check_attributes(src, node, { "name", "value" });
                    std::string name = node.attribute("name").value();
                    std::string value = node.attribute("value").value();
                    if (name.empty())
                        src.throw_error(node, "<default>: name must by nonempty");
                    bool found = false;
                    for (auto &kv: param) {
                        if (kv.first == name)
                            found = true;
                    }
                    if (!found)
                        param.emplace_back(name, value);
                    return std::make_pair("", "");
                }
                break;

            case EInclude: {
                    check_attributes(src, node, { "filename" });
                    ref<FileResolver> fs = Thread::thread()->file_resolver();
                    fs::path filename = fs->resolve(node.attribute("filename").value());
                    if (!fs::exists(filename))
                        src.throw_error(node, "included file \"%s\" not found", filename);

                    Log(Info, "Loading included XML file \"%s\" ..", filename);

                    pugi::xml_document doc;
                    pugi::xml_parse_result result = doc.load_file(filename.native().c_str());

                    detail::XMLSource nested_src {
                        filename.string(), doc,
                        [&](ptrdiff_t pos) { return detail::file_offset(filename, pos); },
                        src.depth + 1
                    };

                    if (nested_src.depth > MTS_XML_INCLUDE_MAX_RECURSION)
                        Throw("Exceeded <include> recursion limit of %i",
                              MTS_XML_INCLUDE_MAX_RECURSION);

                    if (!result) /* There was a parser / file IO error */
                        src.throw_error(node, "error while loading \"%s\" (at %s): %s",
                            nested_src.id, nested_src.offset(result.offset),
                            result.description());

                    try {
                        if (std::string(doc.begin()->name()) == "scene") {
                            for (pugi::xml_node &ch: doc.begin()->children()) {
                                auto [arg_name, nested_id] = parse_xml(nested_src, ctx, ch, parent_tag,
                                          props, param, arg_counter, 1);
                                if (!nested_id.empty())
                                    props.set_named_reference(arg_name, nested_id);
                            }
                        } else {
                            return parse_xml(nested_src, ctx, *doc.begin(), parent_tag,
                                             props, param, arg_counter, 0);
                        }
                    } catch (const std::exception &e) {
                        src.throw_error(node, "%s", e.what());
                    }
                }
                break;

            case EString: {
                    check_attributes(src, node, { "name", "value" });
                    props.set_string(node.attribute("name").value(), node.attribute("value").value());
                }
                break;

            case EFloat: {
                    check_attributes(src, node, { "name", "value" });
                    std::string value = node.attribute("value").value();
                    Float value_float;
                    try {
                        value_float = detail::stof(value);
                    } catch (...) {
                        src.throw_error(node, "could not parse floating point value \"%s\"", value);
                    }
                    props.set_float(node.attribute("name").value(), value_float);
                }
                break;

            case EInteger: {
                    check_attributes(src, node, { "name", "value" });
                    std::string value = node.attribute("value").value();
                    int64_t value_long;
                    try {
                        value_long = detail::stoll(value);
                    } catch (...) {
                        src.throw_error(node, "could not parse integer value \"%s\"", value);
                    }
                    props.set_long(node.attribute("name").value(), value_long);
                }
                break;

            case EBoolean: {
                    check_attributes(src, node, { "name", "value" });
                    std::string value = string::to_lower(node.attribute("value").value());
                    bool result = false;
                    if (value == "true")
                        result = true;
                    else if (value == "false")
                        result = false;
                    else
                        src.throw_error(node, "could not parse boolean value "
                                             "\"%s\" -- must be \"true\" or "
                                             "\"false\"", value);
                    props.set_bool(node.attribute("name").value(), result);
                }
                break;

            case EVector: {
                    detail::expand_value_to_xyz(src, node);
                    check_attributes(src, node, { "name", "x", "y", "z" });
                    props.set_vector3f(node.attribute("name").value(),
                                       detail::parse_vector(src, node));
                }
                break;

            case EPoint: {
                    detail::expand_value_to_xyz(src, node);
                    check_attributes(src, node, { "name", "x", "y", "z" });
                    props.set_point3f(node.attribute("name").value(),
                                      detail::parse_vector(src, node));
                }
                break;

            case EColor : {
                    check_attributes(src, node, { "name", "value" });
                    std::vector<std::string> tokens = string::tokenize(node.attribute("value").value());

                    if (tokens.size() != 3)
                        src.throw_error(node, "'color' tag requires three values (got \"%s\")", node.attribute("value").value());

                    try {
                        Color3f col(detail::stof(tokens[0]),
                                    detail::stof(tokens[1]),
                                    detail::stof(tokens[2]));

                        // Monochrome mode: replace by luminance
                        if (ctx.monochrome)
                            col = luminance(col);

                        props.set_color(node.attribute("name").value(), col);
                    } catch (...) {
                        src.throw_error(node, "could not parse color \"%s\"", node.attribute("value").value());
                    }
                }
                break;

            case ERGB : {
                    check_attributes(src, node, { "name", "value" });
                    std::vector<std::string> tokens = string::tokenize(node.attribute("value").value());

                    if (tokens.size() == 1) {
                        tokens.push_back(tokens[0]);
                        tokens.push_back(tokens[0]);
                    }
                    if (tokens.size() != 3)
                        src.throw_error(node, "'rgb' tag requires one or three values (got \"%s\")", node.attribute("value").value());

                    Properties props2(within_emitter ? "srgb_d65" : "srgb");
                    Color3f col;
                    try {
                        col = Color3f(detail::stof(tokens[0]),
                                      detail::stof(tokens[1]),
                                      detail::stof(tokens[2]));

                        props2.set_color("color", col);
                    } catch (...) {
                        src.throw_error(node, "could not parse RGB value \"%s\"", node.attribute("value").value());
                    }
                    if (!within_emitter && any(col < 0 || col > 1))
                        src.throw_error(node, "invalid RGB reflectance value, must be in the range [0, 1]!");

                    if (!ctx.monochrome) {
                        ref<Object> obj = PluginManager::instance()->create_object(
                            props2, Class::for_name("spectrum", ctx.variant));
                        props.set_object(node.attribute("name").value(), obj);
                    } else {
                        // Monochrome mode: replace by a uniform spectrum
                        Float lum = luminance(props2.color("color"));
                        // if (within_emitter)
                        //     lum /= (MTS_WAVELENGTH_MAX - MTS_WAVELENGTH_MIN);
                        props2 = Properties("uniform");
                        props2.set_float("value", lum);
                        auto obj = PluginManager::instance()->create_object(
                            props2, Class::for_name("spectrum", ctx.variant));
                        props.set_object(node.attribute("name").value(), obj);
                    }
                }
                break;

            case ESpectrum: {
                    check_attributes(src, node, { "name", "value" });
                    std::vector<std::string> tokens = string::tokenize(node.attribute("value").value());

                    if (tokens.size() == 1) {
                        Properties props2(within_emitter ? "d65" : "uniform");
                        try {
                            props2.set_float("value", detail::stof(tokens[0]));
                        } catch (...) {
                            src.throw_error(node, "could not parse constant spectrum \"%s\"", tokens[0]);
                        }

                        // Monochrome mode: replace by a uniform spectrum.
                        if (ctx.monochrome) {
                            props2.set_plugin_name("uniform");
                            props2.set_float("value",
                                             props2.float_("value") /
                                                 (MTS_WAVELENGTH_MAX - MTS_WAVELENGTH_MIN),
                                             false);
                        }

                        ref<Object> obj = PluginManager::instance()->create_object(
                            props2, Class::for_name("spectrum", ctx.variant));
                        auto expanded   = obj->expand();
                        if (expanded.size() == 1)
                            obj = expanded[0];
                        props.set_object(node.attribute("name").value(), obj);

                    } else {
                        /* Parse wavelength:value pairs, where wavelengths are
                           expected to be specified in increasing order.
                           Automatically detect whether wavelengths are regularly
                           sampled, and instantiate the appropriate
                           ContinuousSpectrum plugin. */
                        std::vector<Float> wavelengths, values;
                        bool is_regular = true;
                        Float interval = 0.f;

                        /* Values are scaled so that integrating the
                           spectrum against the CIE curves and converting
                           to sRGB yields (1, 1, 1) for D65. See D65Spectrum. */
                        Float unit_conversion = 1.0f;
                        if (within_emitter)
                            unit_conversion = 100.0f / 10568.0f;

                        for (const std::string &token : tokens) {
                            std::vector<std::string> pair = string::tokenize(token, ":");
                            if (pair.size() != 2)
                                src.throw_error(node, "invalid spectrum (expected wavelength:value pairs)");
                            Float wavelength, value;
                            try {
                                wavelength = detail::stof(pair[0]);
                                value = detail::stof(pair[1]);
                            } catch (...) {
                                src.throw_error(node, "could not parse wavelength:value pair: \"%s\"", token);
                            }

                            value *= unit_conversion;
                            wavelengths.push_back(wavelength);
                            values.push_back(value);

                            size_t n = wavelengths.size();
                            if (n <= 1)
                                continue;
                            Float distance = (wavelengths[n - 1] - wavelengths[n - 2]);
                            if (distance < 0.0f)
                                src.throw_error(node, "wavelengths must be specified in increasing order");
                            if (n == 2)
                                interval = distance;
                            else if ((distance - interval) > math::Epsilon)
                                is_regular = false;
                        }

                        if (!is_regular)
                            Throw("Not implemented yet: irregularly sampled spectra.");

                        Properties props2("interpolated");
                        props2.set_float("lambda_min", wavelengths.front());
                        props2.set_float("lambda_max", wavelengths.back());
                        props2.set_long("size", wavelengths.size());
                        props2.set_pointer("values", values.data());
                        ref<Object> obj = PluginManager::instance()->create_object(
                            props2, Class::for_name("spectrum", ctx.variant));

                        if (!ctx.monochrome) {
                            props.set_object(node.attribute("name").value(), obj);
                        } else {
                            /* Monochrome mode: replace by the equivalent
                               uniform spectrum by pre-integrating against the
                               CIE Y matching curve. */
                            auto *spectrum = (ContinuousSpectrum *) obj.get();
                            Float average  = 0.f;

                            for (Spectrumf wav = MTS_WAVELENGTH_MIN; all(wav <= MTS_WAVELENGTH_MAX);
                                 wav += 1.f) {
                                Spectrumf Yw = cie1931_y(wav);
                                average += (Yw * spectrum->eval(wav)).x();
                            }
                            if (within_emitter)
                                average /= MTS_WAVELENGTH_MAX - MTS_WAVELENGTH_MIN;
                            else
                                /* Divide by integral of the CIE y matching
                                 * curve */
                                average *= 0.0093583f;

                            props2 = Properties("uniform");
                            props2.set_float("value", average);
                            obj = PluginManager::instance()->create_object(
                                props2, Class::for_name("spectrum", ctx.variant));
                            props.set_object(node.attribute("name").value(), obj);
                        }
                    }
                }
                break;

            case ETransform: {
                    check_attributes(src, node, { "name" });
                    ctx.transform = Transform4f();
                }
                break;

            case ERotate: {
                    detail::expand_value_to_xyz(src, node);
                    check_attributes(src, node, { "angle", "x", "y", "z" }, false);
                    Vector3f vec = detail::parse_vector(src, node);
                    std::string angle = node.attribute("angle").value();
                    Float angle_float;
                    try {
                        angle_float = detail::stof(angle);
                    } catch (...) {
                        src.throw_error(node, "could not parse floating point value \"%s\"", angle);
                    }
                    ctx.transform = Transform4f::rotate(vec, angle_float) * ctx.transform;
                }
                break;

            case ETranslate: {
                    detail::expand_value_to_xyz(src, node);
                    check_attributes(src, node, { "x", "y", "z" }, false);
                    Vector3f vec = detail::parse_vector(src, node);
                    ctx.transform = Transform4f::translate(vec) * ctx.transform;
                }
                break;

            case EScale: {
                    detail::expand_value_to_xyz(src, node);
                    check_attributes(src, node, { "x", "y", "z" }, false);
                    Vector3f vec = detail::parse_vector(src, node, 1.f);
                    ctx.transform = Transform4f::scale(vec) * ctx.transform;
                }
                break;

            case ELookAt: {
                    check_attributes(src, node, { "origin", "target", "up" });

                    Point3f origin = parse_named_vector(src, node, "origin");
                    Point3f target = parse_named_vector(src, node, "target");
                    Vector3f up = parse_named_vector(src, node, "up");

                    auto result = Transform4f::look_at(origin, target, up);
                    if (any_nested(enoki::isnan(result.matrix)))
                        src.throw_error(node, "invalid lookat transformation");
                    ctx.transform = result * ctx.transform;
                }
                break;

            case EMatrix: {
                    check_attributes(src, node, { "value" });
                    std::vector<std::string> tokens = string::tokenize(node.attribute("value").value());
                    if (tokens.size() != 16)
                        Throw("matrix: expected 16 values");
                    Matrix4f matrix;
                    for (int i = 0; i < 4; ++i) {
                        for (int j = 0; j < 4; ++j) {
                            try {
                                matrix(i, j) = detail::stof(tokens[i * 4 + j]);
                            } catch (...) {
                                src.throw_error(node, "could not parse floating point value \"%s\"", tokens[i*4 + j]);
                            }
                        }
                    }
                    ctx.transform = Transform4f(matrix) * ctx.transform;
                }
                break;

            default: Throw("Unhandled element \"%s\"", node.name());
        }

        for (pugi::xml_node &ch: node.children())
            parse_xml(src, ctx, ch, tag, props, param, arg_counter, depth + 1);

        if (tag == ETransform)
            props.set_transform(node.attribute("name").value(), ctx.transform);
    } catch (const std::exception &e) {
        if (strstr(e.what(), "Error while loading") == nullptr)
            src.throw_error(node, "%s", e.what());
        else
            throw;
    }

    return std::make_pair("", "");
}

static ref<Object> instantiate_node(XMLParseContext &ctx, const std::string &id) {
    auto it = ctx.instances.find(id);
    if (it == ctx.instances.end())
        Throw("reference to unknown object \"%s\"!", id);

    auto &inst = it->second;
    tbb::spin_mutex::scoped_lock lock(inst.mutex);

    if (inst.object) {
        return inst.object;
    } else if (!inst.alias.empty()) {
        std::string alias = inst.alias;
        lock.release();
        return instantiate_node(ctx, alias);
    }

    Properties &props = inst.props;
    const auto &named_references = props.named_references();

    ThreadEnvironment env;

    tbb::parallel_for(tbb::blocked_range<uint32_t>(
        0u, (uint32_t) named_references.size(), 1),
        [&](const tbb::blocked_range<uint32_t> &range) {
            ScopedSetThreadEnvironment set_env(env);
            for (uint32_t i = range.begin(); i != range.end(); ++i) {
                auto &kv = named_references[i];
                try {
                    ref<Object> obj;

                    // Isolate from parent talks to prevent deadlocks
                    tbb::this_task_arena::isolate([&] {
                        obj = instantiate_node(ctx, kv.second);
                    });

                    // Give the object a chance to recursively expand into sub-objects
                    std::vector<ref<Object>> children = obj->expand();
                    if (children.empty()) {
                        props.set_object(kv.first, obj, false);
                    } else if (children.size() == 1) {
                        props.set_object(kv.first, children[0], false);
                    } else {
                        int ctr = 0;
                        for (auto c : children)
                            props.set_object(kv.first + "_" + std::to_string(ctr++), children[0], false);
                    }
                } catch (const std::exception &e) {
                    if (strstr(e.what(), "Error while loading") == nullptr)
                        Throw("Error while loading \"%s\" (near %s): %s",
                              inst.src_id, inst.offset(inst.location), e.what());
                    else
                        throw;
                }
            }
        }
    );

    try {
        inst.object = PluginManager::instance()->create_object(props, inst.class_);
    } catch (const std::exception &e) {
        Throw("Error while loading \"%s\" (near %s): could not instantiate "
              "%s plugin of type \"%s\": %s", inst.src_id, inst.offset(inst.location),
              string::to_lower(inst.class_->name()), props.plugin_name(),
              e.what());
    }

    auto unqueried = props.unqueried();
    if (!unqueried.empty()) {
        for (auto &v : unqueried) {
            if (props.type(v) == PropertyType::Object) {
                const auto &obj = props.object(v);
                Throw("Error while loading \"%s\" (near %s): unreferenced "
                      "object %s (within %s of type \"%s\")",
                      inst.src_id, inst.offset(inst.location),
                      obj, string::to_lower(inst.class_->name()),
                      inst.props.plugin_name());
            } else {
                v = "\"" + v + "\"";
            }
        }
        Throw("Error while loading \"%s\" (near %s): unreferenced %s "
              "%s in %s plugin of type \"%s\"",
              inst.src_id, inst.offset(inst.location),
              unqueried.size() > 1 ? "properties" : "property", unqueried,
              string::to_lower(inst.class_->name()), props.plugin_name());
    }
    return inst.object;
}

NAMESPACE_END(detail)

ref<Object> load_string(const std::string &string, const std::string &variant,
                        ParameterList param) {
    ScopedPhase sp(EProfilerPhase::EInitScene);
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(string.c_str(), string.length(),
                                                    pugi::parse_default |
                                                    pugi::parse_comments);
    detail::XMLSource src{
        "<string>", doc,
        [&](ptrdiff_t pos) { return detail::string_offset(string, pos); }
    };

    if (!result) /* There was a parser error */
        Throw("Error while loading \"%s\" (at %s): %s", src.id,
              src.offset(result.offset), result.description());

    pugi::xml_node root = doc.document_element();
    detail::XMLParseContext ctx;
    ctx.variant = variant;
    Properties prop("");
    size_t arg_counter; /* Unused */
    auto scene_id = detail::parse_xml(src, ctx, root, EInvalid, prop,
                                      param, arg_counter, 0).second;
    return detail::instantiate_node(ctx, scene_id);
}

ref<Object> load_file(const fs::path &filename_, const std::string &variant,
                      ParameterList param, bool write_update) {
    ScopedPhase sp(EProfilerPhase::EInitScene);
    fs::path filename = filename_;
    if (!fs::exists(filename))
        Throw("\"%s\": file does not exist!", filename);

    Log(Info, "Loading XML file \"%s\" ..", filename);

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filename.native().c_str(),
                                                  pugi::parse_default |
                                                  pugi::parse_comments);

    detail::XMLSource src {
        filename.string(), doc,
        [&](ptrdiff_t pos) { return detail::file_offset(filename, pos); }
    };

    if (!result) /* There was a parser / file IO error */
        Throw("Error while loading \"%s\" (at %s): %s", src.id,
              src.offset(result.offset), result.description());

    pugi::xml_node root = doc.document_element();

    detail::XMLParseContext ctx;
    ctx.variant = variant;
    Properties prop("");
    size_t arg_counter = 0; /* Unused */
    auto scene_id = detail::parse_xml(src, ctx, root, EInvalid, prop,
                                      param, arg_counter, 0).second;

    if (src.modified && write_update) {
        fs::path backup = filename;
        backup.replace_extension(".bak");
        Log(Info, "Writing updated \"%s\" .. (backup at \"%s\")", filename, backup);
        if (!fs::rename(filename, backup))
            Throw("Unable to rename file \"%s\" to \"%s\"!", filename, backup);

        /* Update version number */
        root.prepend_attribute("version").set_value(MTS_VERSION);
        if (root.attribute("type").value() == std::string("scene"))
            root.remove_attribute("type");

        /* Strip anonymous IDs/names */
        for (pugi::xpath_node result2: doc.select_nodes("//*[starts-with(@id, '_unnamed_')]"))
            result2.node().remove_attribute("id");
        for (pugi::xpath_node result2: doc.select_nodes("//*[starts-with(@name, '_arg_')]"))
            result2.node().remove_attribute("name");

        doc.save_file(filename.native().c_str(), "    ");

        /* Update for detail::file_offset */
        filename = backup;
    }

    return detail::instantiate_node(ctx, scene_id);
}

NAMESPACE_END(xml)
NAMESPACE_END(mitsuba)
