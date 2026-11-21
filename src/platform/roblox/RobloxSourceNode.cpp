#include "LSP/JsonRpc.hpp"
#include "Platform/RobloxPlatform.hpp"
#include <queue>

SourceNode::SourceNode(
    std::string name, std::string className, std::vector<std::string> filePaths, std::vector<SourceNode*> children, bool pluginManaged)
    : name(std::move(name))
    , className(std::move(className))
    , filePaths(std::move(filePaths))
    , children(std::move(children))
    , pluginManaged(pluginManaged)
{
}

bool SourceNode::isScript() const
{
    return className == "ModuleScript" || className == "Script" || className == "LocalScript";
}

/// NOTE: Use `WorkspaceFileResolver::getRealPathFromSourceNode()` instead of this function where
/// possible, as that will ensure it is relative to the correct workspace root.
std::optional<std::string> SourceNode::getScriptFilePath() const
{
    for (const auto& path : filePaths)
    {
        const auto uri = Uri::file(path);
        if (uri.extension() == ".lua" || uri.extension() == ".luau")
        {
            return path;
        }
        else if (uri.extension() == ".json" && isScript() && !endsWith(uri.filename(), ".meta.json"))
        {
            return path;
        }
        else if (uri.extension() == ".toml" && isScript())
        {
            return path;
        }
        else if ((uri.extension() == ".yaml" || uri.extension() == ".yml") && isScript())
        {
            return path;
        }
    }
    return std::nullopt;
}

Luau::SourceCode::Type SourceNode::sourceCodeType() const
{
    if (className == "ServerScript" || className == "LocalScript")
    {
        return Luau::SourceCode::Type::Script;
    }
    else if (className == "ModuleScript")
    {
        return Luau::SourceCode::Type::Module;
    }
    else
    {
        return Luau::SourceCode::Type::None;
    }
}

std::optional<SourceNode*> SourceNode::findChild(const std::string& childName) const
{
    for (const auto& child : children)
        if (child->name == childName)
            return child;
    return std::nullopt;
}

std::optional<const SourceNode*> SourceNode::findDescendant(const std::string& childName) const
{
    // Peforms a BFS search
    std::queue<const SourceNode*> queue{};

    // TODO: this isn't so great, we shouldn't really be making a new shared ptr here
    // but since we know this will never get returned, we'll leave it alone for now
    queue.push(this);

    while (!queue.empty())
    {
        auto next = queue.front();
        queue.pop();

        for (const auto& child : next->children)
        {
            if (child->name == childName)
                return child;
            queue.push(child);
        }
    }
    return std::nullopt;
}

bool SourceNode::containsFilePaths() const
{
    return !filePaths.empty() || std::any_of(children.begin(), children.end(),
                                     [](const auto* child)
                                     {
                                         return child->containsFilePaths();
                                     });
}

std::optional<const SourceNode*> SourceNode::findAncestor(const std::string& ancestorName) const
{
    auto current = parent;
    while (current)
    {
        if (current->name == ancestorName)
            return current;
        current = current->parent;
    }
    return std::nullopt;
}

bool SourceNode::isAncestorOf(const SourceNode* other) const
{
    for (auto n = other->parent; n; n = n->parent)
        if (n == this)
            return true;
    return false;
}

const SourceNode* SourceNode::walkPath(const std::string& path) const
{
    const SourceNode* base = this;
    size_t start = 0;
    while (start < path.size())
    {
        if (path.compare(start, 2, "./") == 0)
        {
            start += 2;
            continue;
        }

        size_t end = path.find('/', start);
        std::string segment = (end == std::string::npos) ? path.substr(start) : path.substr(start, end - start);
        start = (end == std::string::npos) ? path.size() : end + 1;

        if (segment.empty() || segment == ".")
            continue;

        if (segment == "..")
        {
            base = base->parent;
            if (!base)
                return nullptr;
        }
        else
        {
            auto child = base->findChild(segment);
            if (!child)
                return nullptr;
            base = *child;
        }
    }

    return base;
}

void SourceNode::clearCachedTypes() const
{
    tys.clear();
    for (const auto& child : children)
        child->clearCachedTypes();
}

namespace
{
// SAX events straight into SourceNodes: a place sourcemap has hundreds of thousands of nodes, and
// building the json DOM first was most of the startup cost. Nodes are allocated in post-order, as
// the recursive DOM walk did. Unknown keys are skipped.
struct SourceNodeSax
{
    using Json = json;
    struct Frame
    {
        std::string name, className;
        bool hasName = false, hasClassName = false, pluginManaged = false;
        std::vector<std::string> filePaths;
        std::vector<SourceNode*> children;
    };
    enum class In
    {
        Node,
        Children,
        FilePaths,
        Skip,
    };

    Luau::TypedAllocator<SourceNode>& allocator;
    std::vector<Frame> frames;
    std::vector<In> in;
    std::string currentKey;
    SourceNode* root = nullptr;

    [[noreturn]] static void fail(const std::string& what)
    {
        throw std::runtime_error("sourcemap: " + what);
    }

    // a scalar inside a node object; `kind` names the value for errors
    void scalar(const char* kind)
    {
        if (in.empty())
            fail("root is not an object");
        switch (in.back())
        {
        case In::Skip:
            return;
        case In::Children:
            fail(std::string("child is ") + kind + ", not an object");
        case In::FilePaths:
            fail(std::string("file path is ") + kind + ", not a string");
        case In::Node:
            if (currentKey == "name" || currentKey == "className" || currentKey == "filePaths" || currentKey == "children" || currentKey == "pluginManaged")
                fail("'" + currentKey + "' is " + kind);
            return;
        }
    }

    bool null()
    {
        scalar("null");
        return true;
    }
    bool boolean(bool b)
    {
        if (!in.empty() && in.back() == In::Node && currentKey == "pluginManaged")
        {
            frames.back().pluginManaged = b;
            return true;
        }
        scalar("a boolean");
        return true;
    }
    bool number_integer(Json::number_integer_t)
    {
        scalar("a number");
        return true;
    }
    bool number_unsigned(Json::number_unsigned_t)
    {
        scalar("a number");
        return true;
    }
    bool number_float(Json::number_float_t, const Json::string_t&)
    {
        scalar("a number");
        return true;
    }
    bool binary(Json::binary_t&)
    {
        scalar("binary");
        return true;
    }
    bool string(Json::string_t& s)
    {
        if (!in.empty() && in.back() == In::FilePaths)
        {
            frames.back().filePaths.emplace_back(std::move(s));
            return true;
        }
        if (!in.empty() && in.back() == In::Node)
        {
            Frame& f = frames.back();
            if (currentKey == "name")
            {
                f.name = std::move(s);
                f.hasName = true;
                return true;
            }
            if (currentKey == "className")
            {
                f.className = std::move(s);
                f.hasClassName = true;
                return true;
            }
        }
        scalar("a string");
        return true;
    }
    bool key(Json::string_t& k)
    {
        if (in.back() == In::Node)
            currentKey = std::move(k);
        return true;
    }
    bool start_object(std::size_t)
    {
        if (in.empty() || in.back() == In::Children)
        {
            if (in.empty() && root)
                fail("more than one root");
            frames.emplace_back();
            in.push_back(In::Node);
            return true;
        }
        if (in.back() == In::Node && (currentKey == "children" || currentKey == "filePaths"))
            fail("'" + currentKey + "' is an object");
        if (in.back() == In::Node && (currentKey == "name" || currentKey == "className" || currentKey == "pluginManaged"))
            fail("'" + currentKey + "' is an object");
        if (in.back() == In::FilePaths)
            fail("file path is an object, not a string");
        in.push_back(In::Skip);
        return true;
    }
    bool end_object()
    {
        In top = in.back();
        in.pop_back();
        if (top == In::Skip)
            return true;
        Frame f = std::move(frames.back());
        frames.pop_back();
        if (!f.hasName)
            fail("node without 'name'");
        if (!f.hasClassName)
            fail("node without 'className'");
        SourceNode* node = allocator.allocate(
            SourceNode(std::move(f.name), std::move(f.className), std::move(f.filePaths), std::move(f.children), f.pluginManaged)
        );
        if (frames.empty())
            root = node;
        else
            frames.back().children.push_back(node);
        return true;
    }
    bool start_array(std::size_t)
    {
        if (in.empty())
            fail("root is not an object");
        if (in.back() == In::Node && currentKey == "children")
            in.push_back(In::Children);
        else if (in.back() == In::Node && currentKey == "filePaths")
            in.push_back(In::FilePaths);
        else if (in.back() == In::Node && (currentKey == "name" || currentKey == "className" || currentKey == "pluginManaged"))
            fail("'" + currentKey + "' is an array");
        else if (in.back() == In::Children)
            fail("child is an array, not an object");
        else if (in.back() == In::FilePaths)
            fail("file path is an array, not a string");
        else
            in.push_back(In::Skip);
        return true;
    }
    bool end_array()
    {
        in.pop_back();
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& e)
    {
        fail(e.what());
    }
};
} // namespace

SourceNode* SourceNode::fromJson(const std::string& contents, Luau::TypedAllocator<SourceNode>& allocator)
{
    SourceNodeSax sax{allocator};
    json::sax_parse(contents, &sax);
    if (!sax.root)
        throw std::runtime_error("sourcemap: root is not an object");
    return sax.root;
}

// Only includes nodes with filepaths to avoid writing every Instance in the DataModel to `sourcemap.json`
ordered_json SourceNode::toJson() const
{
    ordered_json node;
    node["name"] = name;
    node["className"] = className;
    if (pluginManaged)
    {
        // When a plugin-managed node is no longer in the plugin info, it must be pruned.
        // However, when the sourcemap is re-read (ex: file change, reopened editor, LSP restart)
        // that would make all nodes NOT plugin-managed, so nothing could ever be removed after that.
        // Therefore, we need to persist pluginManaged in the json.
        node["pluginManaged"] = pluginManaged;
    }

    if (!filePaths.empty())
    {
        node["filePaths"] = filePaths;
    }

    if (!children.empty())
    {
        ordered_json children_array = ordered_json::array();
        for (const auto* child : children)
        {
            if (!child->containsFilePaths())
            {
                continue;
            }
            children_array.emplace_back(child->toJson());
        }
        if (!children_array.empty())
        {
            node["children"] = children_array;
        }
    }

    return node;
}
