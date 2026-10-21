CXX := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -MMD -MP
LDFLAGS := 

SRC_DIR := src
BUILD_DIR := build
TEST_DIR := tests
BENCH_DIR := benchmarks

TARGET := titan-vm-runtime
TEST_TARGET := test_$(TARGET)
BENCH_TARGET := bench_$(TARGET)

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS := $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/test_%.o, $(TEST_SRCS))

BENCH_SRCS := $(wildcard $(BENCH_DIR)/*.cpp)
BENCH_OBJS := $(patsubst $(BENCH_DIR)/%.cpp, $(BUILD_DIR)/bench_%.o, $(BENCH_SRCS))

CORE_OBJS := $(filter-out $(BUILD_DIR)/main.o, $(OBJS))

.PHONY: all build test clean benchmark directories

all: directories build

directories:
	@mkdir -p $(BUILD_DIR)

build: directories $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

test: directories $(TEST_TARGET)
	@if [ -x "$(BUILD_DIR)/$(TEST_TARGET)" ]; then \
		./$(BUILD_DIR)/$(TEST_TARGET); \
	else \
		echo "No tests found or test target failed to build."; \
	fi

$(TEST_TARGET): $(CORE_OBJS) $(TEST_OBJS)
	@if [ -n "$(TEST_SRCS)" ]; then \
		$(CXX) $(CXXFLAGS) $^ -o $(BUILD_DIR)/$@ $(LDFLAGS); \
	fi

$(BUILD_DIR)/test_%.o: $(TEST_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

benchmark: directories $(BENCH_TARGET)
	@if [ -x "$(BUILD_DIR)/$(BENCH_TARGET)" ]; then \
		./$(BUILD_DIR)/$(BENCH_TARGET); \
	else \
		echo "No benchmarks found or benchmark target failed to build."; \
	fi

$(BENCH_TARGET): $(CORE_OBJS) $(BENCH_OBJS)
	@if [ -n "$(BENCH_SRCS)" ]; then \
		$(CXX) $(CXXFLAGS) $^ -o $(BUILD_DIR)/$@ $(LDFLAGS); \
	fi

$(BUILD_DIR)/bench_%.o: $(BENCH_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)