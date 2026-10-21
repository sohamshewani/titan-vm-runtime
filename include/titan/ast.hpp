#ifndef INCLUDE_TITAN_AST_HPP
#define INCLUDE_TITAN_AST_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <variant>
#include <stdexcept>
#include <utility>

namespace titan {

enum class NodeType : uint8_t {
    Literal,
    Identifier,
    BinaryOp,
    UnaryOp,
    Assignment,
    Block,
    If,
    While,
    FunctionDef,
    Return,
    Call
};

enum class BinaryOperator : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr
};

enum class UnaryOperator : uint8_t {
    Negate,
    LogicalNot,
    BitwiseNot
};

struct SourceLocation {
    uint32_t line;
    uint32_t column;
    uint32_t length;
};

class ArenaBlock {
public:
    explicit ArenaBlock(size_t capacity)
        : capacity_(capacity), offset_(0) {
        memory_ = std::make_unique<char[]>(capacity_);
    }

    ~ArenaBlock() = default;

    ArenaBlock(const ArenaBlock&) = delete;
    ArenaBlock& operator=(const ArenaBlock&) = delete;

    ArenaBlock(ArenaBlock&&) noexcept = default;
    ArenaBlock& operator=(ArenaBlock&&) noexcept = default;

    void* allocate(size_t size, size_t alignment) {
        uintptr_t current_address = reinterpret_ptr<uintptr_t>(memory_.get() + offset_);
        size_t alignment_padding = (alignment - (current_address % alignment)) % alignment;
        size_t total_size = size + alignment_padding;

        if (offset_ + total_size > capacity_) {
            return nullptr;
        }

        offset_ += alignment_padding;
        void* ptr = memory_.get() + offset_;
        offset_ += size;
        return ptr;
    }

    [[nodiscard]] size_t available() const noexcept {
        return capacity_ - offset_;
    }

    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    void reset() noexcept {
        offset_ = 0;
    }

private:
    size_t capacity_;
    size_t offset_;
    std::unique_ptr<char[]> memory_;
};

class ASTArena {
public:
    explicit ASTArena(size_t initial_block_size = 65536)
        : block_size_(initial_block_size) {
        blocks_.emplace_back(block_size_);
    }

    ~ASTArena() = default;

    ASTArena(const ASTArena&) = delete;
    ASTArena& operator=(const ASTArena&) = delete;

    ASTArena(ASTArena&&) noexcept = default;
    ASTArena& operator=(ASTArena&&) noexcept = default;

    template <typename T, typename... Args>
    T* allocate(Args&&... args) {
        constexpr size_t size = sizeof(T);
        constexpr size_t alignment = alignof(T);

        void* mem = blocks_.back().allocate(size, alignment);
        if (mem == nullptr) {
            size_t new_capacity = std::max(block_size_, sizeof(T) + alignof(T));
            blocks_.emplace_back(new_capacity);
            mem = blocks_.back().allocate(size, alignment);
            if (mem == nullptr) {
                throw std::runtime_error("ASTArena allocation failed critically.");
            }
        }

        return new (mem) T(std::forward<Args>(args)...);
    }

    void reset() noexcept {
        blocks_.clear();
        blocks_.emplace_back(block_size_);
    }

    [[nodiscard]] size_t total_allocated_bytes() const noexcept {
        size_t total = 0;
        for (const auto& block : blocks_) {
            total += block.capacity();
        }
        return total;
    }

private:
    size_t block_size_;
    std::vector<ArenaBlock> blocks_;
};

class ASTNode {
public:
    explicit ASTNode(NodeType type, SourceLocation loc)
        : type_(type), location_(loc) {}

    virtual ~ASTNode() = default;

    [[nodiscard]] NodeType type() const noexcept { return type_; }
    [[nodiscard]] SourceLocation location() const noexcept { return location_; }

private:
    NodeType type_;
    SourceLocation location_;
};

class LiteralNode : public ASTNode {
public:
    explicit LiteralNode(uint64_t val_repr, SourceLocation loc)
        : ASTNode(NodeType::Literal, loc), value_repr_(val_repr) {}

    [[nodiscard]] uint64_t value() const noexcept { return value_repr_; }

private:
    uint64_t value_repr_;
};

class IdentifierNode : public ASTNode {
public:
    IdentifierNode(std::string_view name, SourceLocation loc)
        : ASTNode(NodeType::Identifier, loc), name_(name) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    std::string_view name_;
};

class BinaryOpNode : public ASTNode {
public:
    BinaryOpNode(BinaryOperator op, ASTNode* lhs, ASTNode* rhs, SourceLocation loc)
        : ASTNode(NodeType::BinaryOp, loc), op_(op), lhs_(lhs), rhs_(rhs) {}

    [[nodiscard]] BinaryOperator op() const noexcept { return op_; }
    [[nodiscard]] ASTNode* lhs() const noexcept { return lhs_; }
    [[nodiscard]] ASTNode* rhs() const noexcept { return rhs_; }

private:
    BinaryOperator op_;
    ASTNode* lhs_;
    ASTNode* rhs_;
};

class UnaryOpNode : public ASTNode {
public:
    UnaryOpNode(UnaryOperator op, ASTNode* operand, SourceLocation loc)
        : ASTNode(NodeType::UnaryOp, loc), op_(op), operand_(operand) {}

    [[nodiscard]] UnaryOperator op() const noexcept { return op_; }
    [[nodiscard]] ASTNode* operand() const noexcept { return operand_; }

private:
    UnaryOperator op_;
    ASTNode* operand_;
};

class AssignmentNode : public ASTNode {
public:
    AssignmentNode(std::string_view name, ASTNode* value, SourceLocation loc)
        : ASTNode(NodeType::Assignment, loc), name_(name), value_(value) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] ASTNode* value() const noexcept { return value_; }

private:
    std::string_view name_;
    ASTNode* value_;
};

class BlockNode : public ASTNode {
public:
    explicit BlockNode(SourceLocation loc)
        : ASTNode(NodeType::Block, loc) {}

    void add_statement(ASTNode* stmt) {
        statements_.push_back(stmt);
    }

    [[nodiscard]] const std::vector<ASTNode*>& statements() const noexcept {
        return statements_;
    }

private:
    std::vector<ASTNode*> statements_;
};

class IfNode : public ASTNode {
public:
    IfNode(ASTNode* condition, ASTNode* then_branch, ASTNode* else_branch, SourceLocation loc)
        : ASTNode(NodeType::If, loc), condition_(condition), then_branch_(then_branch), else_branch_(else_branch) {}

    [[nodiscard]] ASTNode* condition() const noexcept { return condition_; }
    [[nodiscard]] ASTNode* then_branch() const noexcept { return then_branch_; }
    [[nodiscard]] ASTNode* else_branch() const noexcept { return else_branch_; }

private:
    ASTNode* condition_;
    ASTNode* then_branch_;
    ASTNode* else_branch_;
};

class WhileNode : public ASTNode {
public:
    WhileNode(ASTNode* condition, ASTNode* body, SourceLocation loc)
        : ASTNode(NodeType::While, loc), condition_(condition), body_(body) {}

    [[nodiscard]] ASTNode* condition() const noexcept { return condition_; }
    [[nodiscard]] ASTNode* body() const noexcept { return body_; }

private:
    ASTNode* condition_;
    ASTNode* body_;
};

class FunctionDefNode : public ASTNode {
public:
    FunctionDefNode(std::string_view name, std::vector<std::string_view> params, ASTNode* body, SourceLocation loc)
        : ASTNode(NodeType::FunctionDef, loc), name_(name), params_(std::move(params)), body_(body) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<std::string_view>& params() const noexcept { return params_; }
    [[nodiscard]] ASTNode* body() const noexcept { return body_; }

private:
    std::string_view name_;
    std::vector<std::string_view> params_;
    ASTNode* body_;
};

class ReturnNode : public ASTNode {
public:
    ReturnNode(ASTNode* value, SourceLocation loc)
        : ASTNode(NodeType::Return, loc), value_(value) {}

    [[nodiscard]] ASTNode* value() const noexcept { return value_; }

private:
    ASTNode* value_;
};

class CallNode : public ASTNode {
public:
    CallNode(ASTNode* callee, std::vector<ASTNode*> args, SourceLocation loc)
        : ASTNode(NodeType::Call, loc), callee_(callee), args_(std::move(args)) {}

    [[nodiscard]] ASTNode* callee() const noexcept { return callee_; }
    [[nodiscard]] const std::vector<ASTNode*>& args() const noexcept { return args_; }

private:
    ASTNode* callee_;
    std::vector<ASTNode*> args_;
};

} // namespace titan

#endif // INCLUDE_TITAN_AST_HPP