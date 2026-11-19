#include "DeclReader.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace survival_shield {
namespace {

// Los decls de texto de mods comunitarios reales llegan a ~2 MB (p. ej.
// modelskinned/player/doomslayer_1p/base/tp_body.decl de "All Heavy Hidden
// Front Entries", 1,99 MB); 8 MB da margen sin abrir la puerta a binarios.
constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;

struct Token {
    enum class Kind { Word, String, Punct };
    Kind kind;
    std::string text;
};

bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool IsWordChar(char c) noexcept {
    // item[0], rutas ui/base/menu_screen, ids con punto/guion: todo es
    // palabra hasta un separador real.
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' ||
           c == '/' || c == '[' || c == ']' || c == '-' || c == '+' || c == ':' ||
           c == '$' || c == '@';
}

bool IsPunct(char c) noexcept {
    return c == '{' || c == '}' || c == '=' || c == ';';
}

void Lex(const std::string& text, std::vector<Token>& out) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char current = text[i];
        if (IsSpace(current)) {
            ++i;
            continue;
        }
        if (current == ',' || current == '(' || current == ')') {
            ++i;
            continue;
        }
        if (current == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (current == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                ++i;
            }
            i = (i + 1 < text.size()) ? i + 2 : text.size();
            continue;
        }
        if (current == '"') {
            ++i;
            std::string value;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                }
                value.push_back(text[i++]);
            }
            if (i < text.size()) {
                ++i; // comilla de cierre
            }
            out.push_back({Token::Kind::String, std::move(value)});
            continue;
        }
        if (IsPunct(current)) {
            out.push_back({Token::Kind::Punct, std::string(1, current)});
            ++i;
            continue;
        }
        std::string word;
        while (i < text.size() && !IsSpace(text[i]) && !IsPunct(text[i]) &&
               text[i] != '"' && text[i] != ',') {
            word.push_back(text[i++]);
        }
        if (!word.empty()) {
            out.push_back({Token::Kind::Word, std::move(word)});
        }
    }
}

struct Node {
    std::string key;
    std::string scalar;
    bool block = false;
    std::vector<Node> children;
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    // Parsea un bloque empezando en un '{'. Consume su '}' de cierre.
    bool ParseBlock(std::vector<Node>* out) {
        if (!Expect("{")) {
            return false;
        }
        ++position_;
        return ParseStatements(out, true);
    }

    // Decl "plana": sentencias key = value al nivel raíz, sin llaves que las
    // envuelvan (p. ej. entityDef/syncmelee/playervsai/tyrant.decl).
    bool ParseFlat(std::vector<Node>* out) {
        return ParseStatements(out, false);
    }

private:
    bool ParseStatements(std::vector<Node>* out, bool insideBlock) {
        while (position_ < tokens_.size()) {
            if (insideBlock && tokens_[position_].text == "}") {
                ++position_;
                return true;
            }
            const Token& token = tokens_[position_];
            if (token.kind == Token::Kind::Punct && token.text == ";") {
                ++position_;
                continue;
            }
            if (token.kind != Token::Kind::Word && token.kind != Token::Kind::String) {
                if (insideBlock) {
                    return false; // bloque malformado
                }
                ++position_; // tolerante en raíz: token suelto, se salta
                continue;
            }
            Node node;
            node.key = token.text;
            ++position_;
            if (position_ >= tokens_.size()) {
                return !insideBlock;
            }
            if (tokens_[position_].text == "=") {
                ++position_;
                if (position_ >= tokens_.size()) {
                    return false;
                }
                if (tokens_[position_].text == "{") {
                    node.block = true;
                    if (!ParseBlock(&node.children)) {
                        return false;
                    }
                } else if (tokens_[position_].text == "[") {
                    if (!SkipArray()) {
                        return false;
                    }
                } else if (tokens_[position_].kind == Token::Kind::String ||
                           tokens_[position_].kind == Token::Kind::Word) {
                    node.scalar = tokens_[position_].text;
                    ++position_;
                } else {
                    ++position_;
                }
            } else if (tokens_[position_].text == "{") {
                node.block = true;
                if (!ParseBlock(&node.children)) {
                    return false;
                }
            }
            if (position_ < tokens_.size() && tokens_[position_].text == ";") {
                ++position_;
            }
            out->push_back(std::move(node));
        }
        return !insideBlock; // dentro de un bloque: se acabó el texto sin '}'
    }

    bool Expect(const char* text) const {
        return position_ < tokens_.size() && tokens_[position_].text == text;
    }

    bool SkipArray() {
        if (!Expect("[")) {
            return false;
        }
        ++position_;
        int depth = 1;
        while (position_ < tokens_.size() && depth > 0) {
            if (tokens_[position_].text == "[") {
                ++depth;
            } else if (tokens_[position_].text == "]") {
                --depth;
            }
            ++position_;
        }
        return depth == 0;
    }

    const std::vector<Token>& tokens_;
    std::size_t position_ = 0;
};

const Node* FindChild(const std::vector<Node>& nodes, const char* key) {
    for (const Node& node : nodes) {
        if (node.key == key) {
            return &node;
        }
    }
    return nullptr;
}

void FlattenProps(const std::vector<Node>& nodes, const std::string& prefix,
                  std::vector<std::string>* out) {
    for (const Node& node : nodes) {
        if (out->size() >= DeclReader::kMaxProps) {
            return;
        }
        const std::string path = prefix.empty() ? node.key : prefix + "." + node.key;
        if (node.block) {
            FlattenProps(node.children, path, out);
        } else {
            out->push_back(path);
        }
    }
}

} // namespace

bool DeclReader::ParseText(const std::string& text, DeclInfo* out) {
    if (out == nullptr) {
        return false;
    }
    std::vector<Token> tokens;
    tokens.reserve(256);
    Lex(text, tokens);
    if (tokens.empty()) {
        return false;
    }

    std::vector<Node> root;
    Parser parser(tokens);
    if (tokens.front().text == "{") {
        if (!parser.ParseBlock(&root)) {
            return false;
        }
    } else {
        if (!parser.ParseFlat(&root)) {
            return false;
        }
        // Texto plano cualquiera también "parsea" como sentencias sueltas;
        // exigir al menos una asignación real (key = valor/bloque).
        const bool hasAssignment = std::any_of(
            root.begin(), root.end(), [](const Node& node) {
                return node.block || !node.scalar.empty();
            });
        if (!hasAssignment) {
            return false;
        }
    }

    if (const Node* inherit = FindChild(root, "inherit")) {
        out->inherit = inherit->scalar;
    }
    const Node* edit = FindChild(root, "edit");
    const std::vector<Node>& editNodes = edit != nullptr ? edit->children : root;
    if (const Node* className = FindChild(editNodes, "class")) {
        out->className = className->scalar;
    }
    // Las rutas se reportan ancladas a la raíz del decl: "edit.model....".
    FlattenProps(editNodes, edit != nullptr ? "edit" : "", &out->props);
    return true;
}

bool DeclReader::ParseFile(const std::filesystem::path& declsRoot,
                           const std::filesystem::path& file, DeclInfo* out) {
    if (out == nullptr) {
        return false;
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > kMaxFileBytes) {
        return false;
    }

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    if (!ParseText(text, out)) {
        return false;
    }

    const auto relative = std::filesystem::relative(file, declsRoot, error);
    if (error) {
        return false;
    }
    bool first = true;
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        std::string part = it->string();
        const bool last = std::next(it) == relative.end();
        if (last && part.size() > 5 && part.rfind(".decl") == part.size() - 5) {
            part.resize(part.size() - 5); // último tramo: sin extensión
        }
        if (first) {
            out->type = part;
            first = false;
        } else if (out->name.empty()) {
            out->name = part;
        } else {
            out->name += "/" + part;
        }
    }
    return !out->name.empty();
}

} // namespace survival_shield
