#ifndef TITAN_COMPILER_HPP
#define TITAN_COMPILER_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <array>

namespace titan {

// Forward declarations for VM and runtime structures
class ArenaBlock;
class HeapObject;
template <typename T> class GCPtr;

namespace vm {
    enum class Opcode : uint8_t {
        LoadConst = 0x01,
        LoadNil,
        LoadTrue,
        LoadFalse,
        Add,
        Sub,
        Mul,
        Div,
        Equal,
        Less,
        Jump,
        JumpIfFalse,
        Call,
        Return,
        StoreLocal,
        LoadLocal,
        StoreGlobal,
        LoadGlobal,
        Halt
    };

    struct alignas(64) Instruction {
        uint32_t raw;

        static constexpr uint32_t encode(Opcode op, uint8_t a, uint16_t b) {
            return static_cast<uint32_t>(op) | 
                   (static_cast<uint32_t>(a) << 8) | 
                   (static_cast<uint32_t>(b) << 16);
        }

        static constexpr uint32_t encode_ab(Opcode op, uint8_t a, uint8_t b) {
            return static_cast<uint32_t>(op) | 
                   (static_cast<uint32_t>(a) << 8) | 
                   (static_cast<uint32_t>(b) << 16);
        }
    };
}

namespace ast {
    enum class NodeType {
        LiteralInt,
        LiteralFloat,
        LiteralBool,
        LiteralString,
        Identifier,
        BinaryExpr,
        UnaryExpr,
        AssignExpr,
        BlockStmt,
        IfStmt,
        WhileStmt,
        FunctionDecl,
        ReturnStmt,
        ExpressionStmt
    };

    enum class TokenType {
        End = 0,
        Identifier,
        NumberInt,
        NumberFloat,
        String,
        Plus,
        Minus,
        Star,
        Slash,
        Equal,
        EqualEqual,
        BangEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        LeftParen,
        RightParen,
        LeftBrace,
        RightBrace,
        Comma,
        Semicolon,
        KwIf,
        KwElse,
        KwWhile,
        KwFn,
        KwReturn,
        KwTrue,
        KwFalse,
        KwNil
    };

    struct Token {
        TokenType type;
        std::string_view lexeme;
        size_t line;
        size_t column;
    };

    struct SourceLocation {
        size_t line;
        size_t column;
    };

    class ASTNode {
    public:
        SourceLocation loc;
        explicit ASTNode(SourceLocation l) : loc(l) {}
        virtual ~ASTNode() = default;
        virtual NodeType get_type() const = 0;
    };

    class LiteralIntNode : public ASTNode {
    public:
        int64_t value;
        LiteralIntNode(SourceLocation l, int64_t val) : ASTNode(l), value(val) {}
        NodeType get_type() const override { return NodeType::LiteralInt; }
    };

    class LiteralFloatNode : public ASTNode {
    public:
        double value;
        LiteralFloatNode(SourceLocation l, double val) : ASTNode(l), value(val) {}
        NodeType get_type() const override { return NodeType::LiteralFloat; }
    };

    class LiteralBoolNode : public ASTNode {
    public:
        bool value;
        LiteralBoolNode(SourceLocation l, bool val) : ASTNode(l), value(val) {}
        NodeType get_type() const override { return NodeType::LiteralBool; }
    };

    class LiteralStringNode : public ASTNode {
    public:
        std::string value;
        LiteralStringNode(SourceLocation l, std::string val) : ASTNode(l), std::move(val), value(val) {}
        NodeType get_type() const override { return NodeType::LiteralString; }
    };

    class IdentifierNode : public ASTNode {
    public:
        std::string name;
        IdentifierNode(SourceLocation l, std::string n) : ASTNode(l), name(std::move(n)) {}
        NodeType get_type() const override { return NodeType::Identifier; }
    };

    class BinaryExprNode : public ASTNode {
    public:
        TokenType op;
        ASTNode* left;
        ASTNode* right;
        BinaryExprNode(SourceLocation l, TokenType o, ASTNode* lhs, ASTNode* rhs)
            : ASTNode(l), op(o), left(lhs), right(rhs) {}
        NodeType get_type() const override { return NodeType::BinaryExpr; }
    };

    class BlockStmtNode : public ASTNode {
    public:
        std::vector<ASTNode*> statements;
        explicit BlockStmtNode(SourceLocation l) : ASTNode(l) {}
        NodeType get_type() const override { return NodeType::BlockStmt; }
    };

    class FunctionDeclNode : public ASTNode {
    public:
        std::string name;
        std::vector<std::string> parameters;
        BlockStmtNode* body;
        FunctionDeclNode(SourceLocation l, std::string n, std::vector<std::string> params, BlockStmtNode* b)
            : ASTNode(l), name(std::move(n)), parameters(std::move(params)), body(b) {}
        NodeType get_type() const override { return NodeType::FunctionDecl; }
    };
}

class ArenaAllocator {
private:
    struct Block {
        std::unique_ptr<uint8_t[]> memory;
        size_t capacity;
        size_t offset;
        Block* next;

        Block(size_t cap) : capacity(cap), offset(0), next(nullptr) {
            memory = std::make_unique<uint8_t[]>(capacity);
        }
    };

    Block* current_block_;
    size_t default_block_size_;

public:
    explicit ArenaAllocator(size_t block_size = 65536) 
        : default_block_size_(block_size) {
        current_block_ = new Block(default_block_size_);
    }

    ~ArenaAllocator() {
        Block* curr = current_block_;
        while (curr != nullptr) {
            Block* next = curr->next;
            delete curr;
            curr = next;
        }
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        size_t size = sizeof(T);
        size_t align = alignof(T);
        
        size_t current_addr = reinterpret_cast<size_t>(current_block_->memory.get() + current_block_->offset);
        size_t aligned_addr = (current_addr + (align - 1)) & ~(align - 1);
        size_t padding = aligned_addr - current_addr;
        
        if (current_block_->offset + padding + size > current_block_->capacity) {
            size_t new_capacity = std::max(default_block_size_, size + align);
            Block* new_block = new Block(new_capacity);
            new_block->next = current_block_;
            current_block_ = new_block;
            
            current_addr = reinterpret_cast<size_t>(current_block_->memory.get());
            aligned_addr = (current_addr + (align - 1)) & ~(align - 1);
            padding = aligned_addr - current_addr;
        }
        
        void* ptr = reinterpret_cast<void*>(aligned_addr);
        current_block_->offset += padding + size;
        
        return new (ptr) T(std::forward<Args>(args)...);
    }

    void reset() {
        Block* curr = current_block_->next;
        while (curr != nullptr) {
            Block* next = curr->next;
            delete curr;
            curr = next;
        }
        current_block_->next = nullptr;
        current_block_->offset = 0;
    }
};

class Lexer {
private:
    std::string_view source_;
    size_t start_;
    size_t current_;
    size_t line_;
    size_t column_;

    bool is_at_end() const {
        return current_ >= source_.length();
    }

    char advance() {
        column_++;
        return source_[current_++];
    }

    char peek() const {
        if (is_at_end()) return '\0';
        return source_[current_];
    }

    char peek_next() const {
        if (current_ + 1 >= source_.length()) return '\0';
        return source_[current_ + 1];
    }

    bool match(char expected) {
        if (is_at_end()) return false;
        if (source_[current_] != expected) return false;
        current_++;
        column_++;
        return true;
    }

    ast::Token make_token(ast::TokenType type) const {
        std::string_view lexeme = source_.substr(start_, current_ - start_);
        return ast::Token{type, lexeme, line_, column_ - (current_ - start_)};
    }

    ast::Token error_token(std::string_view message) const {
        return ast::Token{ast::TokenType::End, message, line_, column_};
    }

    void skip_whitespace() {
        for (;;) {
            char c = peek();
            switch (c) {
                case ' ':
                case '\r':
                case '\t':
                    advance();
                    break;
                case '\n':
                    line_++;
                    column_ = 0;
                    advance();
                    break;
                case '/':
                    if (peek_next() == '/') {
                        while (peek() != '\n' && !is_at_end()) advance();
                    } else {
                        return;
                    }
                    break;
                default:
                    return;
            }
        }
    }

    ast::Token string_literal() {
        while (peek() != '"' && !is_at_end()) {
            if (peek() == '\n') {
                line_++;
                column_ = 0;
            }
            advance();
        }

        if (is_at_end()) {
            return error_token("Unterminated string.");
        }

        advance(); // Closing quote
        return make_token(ast::TokenType::String);
    }

    ast::Token number_literal() {
        bool is_float = false;
        while (std::isdigit(peek())) advance();

        if (peek() == '.' && std::isdigit(peek_next())) {
            is_float = true;
            advance(); // Consume '.'
            while (std::isdigit(peek())) advance();
        }

        return make_token(is_float ? ast::TokenType::NumberFloat : ast::TokenType::NumberInt);
    }

    ast::Token identifier() {
        while (std::isalnum(peek()) || peek() == '_') advance();

        std::string_view text = source_.substr(start_, current_ - start_);
        ast::TokenType type = ast::TokenType::Identifier;

        if (text == "if") type = ast::TokenType::KwIf;
        else if (text == "else") type = ast::TokenType::KwElse;
        else if (text == "while") type = ast::TokenType::KwWhile;
        else if (text == "fn") type = ast::TokenType::KwFn;
        else if (text == "return") type = ast::TokenType::KwReturn;
        else if (text == "true") type = ast::TokenType::KwTrue;
        else if (text == "false") type = ast::TokenType::KwFalse;
        else if (text == "nil") type = ast::TokenType::KwNil;

        return make_token(type);
    }

public:
    explicit Lexer(std::string_view source)
        : source_(source), start_(0), current_(0), line_(1), column_(0) {}

    ast::Token scan_token() {
        skip_whitespace();
        start_ = current_;

        if (is_at_end()) return make_token(ast::TokenType::End);

        char c = advance();

        if (std::isalpha(c) || c == '_') return identifier();
        if (std::isdigit(c)) return number_literal();

        switch (c) {
            case '(': return make_token(ast::TokenType::LeftParen);
            case ')': return make_token(ast::TokenType::RightParen);
            case '{': return make_token(ast::TokenType::LeftBrace);
            case '}': return make_token(ast::TokenType::RightBrace);
            case ',': return make_token(ast::TokenType::Comma);
            case ';': return make_token(ast::TokenType::Semicolon);
            case '+': return make_token(ast::TokenType::Plus);
            case '-': return make_token(ast::TokenType::Minus);
            case '*': return make_token(ast::TokenType::Star);
            case '/': return make_token(ast::TokenType::Slash);
            case '=': 
                return make_token(match('=') ? ast::TokenType::EqualEqual : ast::TokenType::Equal);
            case '!': 
                return make_token(match('=') ? ast::TokenType::BangEqual : ast::TokenType::End);
            case '<': 
                return make_token(match('=') ? ast::TokenType::LessEqual : ast::TokenType::Less);
            case '>': 
                return make_token(match('=') ? ast::TokenType::GreaterEqual : ast::TokenType::Greater);
            case '"': return string_literal();
            default:
                return error_token("Unexpected character.");
        }
    }
};

class Parser {
private:
    Lexer lexer_;
    ArenaAllocator& arena_;
    ast::Token current_;
    ast::Token previous_;
    bool had_error_;
    bool panic_mode_;

    void error_at(const ast::Token& token, std::string_view message) {
        if (panic_mode_) return;
        panic_mode_ = true;
        std::string err_msg = "Compilation Error at line " + std::to_string(token.line) + 
                              ", col " + std::to_string(token.column);
        if (token.type == ast::TokenType::End) {
            err_msg += " at end";
        } else {
            err_msg += " at '";
            err_msg.append(token.lexeme.data(), token.lexeme.size());
            err_msg += "'";
        }
        err_msg += ": ";
        err_msg.append(message.data(), message.size());
        throw std::runtime_error(err_msg);
    }

    void error(std::string_view message) {
        error_at(current_, message);
    }

    void advance() {
        previous_ = current_;
        for (;;) {
            current_ = lexer_.scan_token();
            if (current_.type != ast::TokenType::End || current_.lexeme.empty()) break;
            error_at(current_, current_.lexeme);
        }
    }

    void consume(ast::TokenType type, std::string_view message) {
        if (current_.type == type) {
            advance();
            return;
        }
        error(message);
    }

    bool check(ast::TokenType type) const {
        return current_.type == type;
    }

    bool match(ast::TokenType type) {
        if (!check(type)) return false;
        advance();
        return true;
    }

    ast::ASTNode* parse_literal() {
        ast::SourceLocation loc{current_.line, current_.column};
        if (match(ast::TokenType::KwTrue)) return arena_.allocate<ast::LiteralBoolNode>(loc, true);
        if (match(ast::TokenType::KwFalse)) return arena_.allocate<ast::LiteralBoolNode>(loc, false);
        if (match(ast::TokenType::KwNil)) return arena_.allocate<ast::LiteralBoolNode>(loc, false); // Nil representation
        
        if (match