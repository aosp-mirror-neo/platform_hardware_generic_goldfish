// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

#include "goldfish/parsing/arg_stream.h"
#include "goldfish/parsing/parameter_binder.h"
#include "line_command_handler.h"

namespace goldfish::telnet {
using ArgStream = goldfish::parsing::ArgStream;

/**
 * @brief A registry for emulator console commands.
 *
 * CommandRegistry acts as the execution engine for console interactions. It
 * manages a tree of command handlers and dispatches incoming command lines
 * to the appropriate logic based on the command name and sub-commands.
 *
 * This class is immutable after construction. Use `CommandRegistryBuilder`
 * to define the command hierarchy and create an instance. Multiple names
 * for a single command (aliases) can be registered by separating them with
 * the pipe character (`|`), e.g., "quit|exit".
 *
 * The built-in help commands (`help`, `h`, `?`, and `help-verbose`) are
 * automatically registered.
 *
 * ### Example Usage:
 * @code
 *   // 1. Define the command tree using a builder
 *   CommandRegistryBuilder builder("/path/to/token");
 *   builder.Command("ping", "Check connection")
 *          .Safe()
 *          .Handler([](auto& ctx, auto& args) { return "pong"; });
 *
 *   // 2. Using a custom context with automatic type-safe dispatch via 'On'
 *   struct MyContext : public LineCommandHandler::Context {
 *       std::string user = "admin";
 *   };
 *   builder.Command("whoami", "Show user info")
 *          .On("id", "show user id", [](MyContext& ctx) {
 *              return ctx.user; // Automatically cast to MyContext
 *          });
 *
 *   // 3. Command with typed and optional parameters
 *   builder.Command("set-location", "Set device coordinates")
 *          .On("gps", "set lat and optional lon",
 *              "'geo fix <lon> <lat>': set the coordinates",
 *              [](LineCommandHandler::Context&, double lat, std::optional<double> lon) {
 *                  return absl::StrCat("Lat: ", lat, " Lon: ", lon.value_or(0.0));
 *              });
 *
 *   auto registry = builder.Build();
 *
 *   // 4. Process commands using the registry
 *   MyContext ctx;
 *   auto result = (*registry)("whoami id", ctx);
 *   if (result.ok()) {
 *       // Handle success: *result contains "admin"
 *   }
 *
 *   auto gps_result = (*registry)("set-location gps 12.3 45.6", ctx);
 * @endcode
 */
class CommandRegistry : public LineCommandHandler {
  private:
    struct Passkey {};

  public:
    friend class CommandRegistryBuilder;

    ~CommandRegistry() override = default;

    // LineCommandHandler implementation
    absl::StatusOr<std::string> operator()(std::string line, Context& ctx) override;
    std::string WelcomeMessage(const Context& ctx) const override;

    using CommandHandler =
            std::function<absl::StatusOr<std::string>(LineCommandHandler::Context&, ArgStream&)>;
    using CommandObserver =
            std::function<void(std::string_view top_level_name, std::string_view canonical_path)>;

    /**
     * @brief Internal representation of a command node in the tree.
     */
    struct Entry {
        std::string name;         ///< Invocation name(s), e.g., "quit|exit".
        std::string abstract;     ///< Short description for help listings.
        std::string description;  ///< Detailed help text for 'help <cmd>'.
        CommandHandler handler;   ///< Optional execution logic.
        bool is_safe = false;     ///< True if executable while unauthenticated.
        std::vector<std::unique_ptr<Entry>> children;  ///< Nested sub-commands.
    };

    explicit CommandRegistry(Passkey, std::filesystem::path token_path,
                             std::vector<std::unique_ptr<Entry>> root_commands,
                             CommandObserver command_observer = nullptr);

    static bool Matches(const Entry& entry, std::string_view name);
    static std::string_view GetCanonicalName(std::string_view names);
    static const Entry* FindChild(const std::vector<std::unique_ptr<Entry>>& children,
                                  std::string_view name);

  private:
    absl::StatusOr<std::string> DispatchCommand(ArgStream& args, Context& ctx) const;
    absl::StatusOr<std::string> ShowHelp(ArgStream& args, Context& ctx, bool verbose) const;
    std::string ShowRootHelp(const Context& ctx, bool verbose) const;
    absl::StatusOr<std::string> ShowSubcommandHelp(ArgStream& args, const Context& ctx) const;
    static std::string GetSubCommandsHelp(const Entry& entry, std::string_view path,
                                          const Context& ctx, bool verbose = true);

    const std::filesystem::path token_path_;
    std::vector<std::unique_ptr<Entry>> root_commands_;
    CommandObserver command_observer_;
};

/**
 * @brief Builder for creating a CommandRegistry.
 *
 * Provides methods for defining a hierarchical command tree. The resulting
 * CommandRegistry is immutable and thread-safe for reading.
 */
class CommandRegistryBuilder {
  public:
    explicit CommandRegistryBuilder(std::filesystem::path token_path,
                                    CommandRegistry::CommandObserver command_observer = nullptr);

    /**
     * @brief Helper to configure a specific command node.
     */
    class NodeBuilder {
      public:
        /**
         * @brief Marks the current command as executable without authentication.
         * @return Reference to this NodeBuilder for chaining.
         */
        NodeBuilder& Safe();

        /**
         * @brief Registers a raw execution handler for the current command.
         * @param handler The logic to execute when this command is invoked.
         * @return Reference to this NodeBuilder for chaining.
         */
        NodeBuilder& Handler(CommandRegistry::CommandHandler handler);

        /**
         * @brief Registers a command with automatic argument parsing.
         *
         * The handler's signature determines which arguments are extracted
         * from the input stream. Supported types include `int`, `double`,
         * `bool`, `std::string`, and `std::optional<T>`.
         *
         * @param name The name of the command (can include '|' for aliases).
         * @param abstract Short description for parent help listings.
         * @param handler The callable logic to execute.
         * @return Reference to this NodeBuilder for chaining.
         */
        template <typename F>
        NodeBuilder& On(std::string name, std::string abstract, F&& handler) {
            return On(std::move(name), std::move(abstract), "", std::forward<F>(handler));
        }

        /**
         * @brief Registers a sub-command with automatic parsing and detailed help.
         *
         * The handler's signature determines which arguments are extracted
         * from the input stream. Supported types include `int`, `double`,
         * `bool`, `std::string`, and `std::optional<T>`.
         *
         * @param name The name of the command.
         * @param abstract Short description for parent help listings.
         * @param description Detailed help text for 'help <this-command>'.
         * @param handler The callable logic to execute.
         * @return Reference to this NodeBuilder for chaining.
         */
        template <typename F>
        NodeBuilder& On(std::string name, std::string abstract, std::string description,
                        F&& handler) {
            return OnInternal(std::move(name), std::move(abstract), std::move(description),
                              WrapHandler(std::forward<F>(handler)));
        }

        /**
         * @brief Creates or focuses on a sub-command group.
         * @param name The name of the sub-command group.
         * @param abstract Short description for parent help listings.
         * @return A NodeBuilder focused on the sub-command node.
         */
        NodeBuilder Sub(std::string name, std::string abstract);

        /**
         * @brief Creates or focuses on a sub-command group with detailed help.
         * @param name The name of the sub-command group.
         * @param abstract Short description for parent help listings.
         * @param description Detailed help text for 'help <this-group>'.
         * @return A NodeBuilder focused on the sub-command node.
         */
        NodeBuilder Sub(std::string name, std::string abstract, std::string description);

      private:
        friend class CommandRegistryBuilder;
        explicit NodeBuilder(CommandRegistry::Entry* entry);
        NodeBuilder& OnInternal(std::string name, std::string abstract, std::string description,
                                CommandRegistry::CommandHandler handler);

        CommandRegistry::Entry* entry_;
    };

    /**
     * @brief Starts the definition of a root-level command.
     *
     * @param name The name of the command (aliases can be separated by '|').
     * @param abstract A brief description for root help listings.
     * @return A NodeBuilder focused on the root command node.
     */
    NodeBuilder Command(std::string name, std::string abstract);

    /**
     * @brief Starts the definition of a root-level command with detailed help.
     *
     * @param name The name of the command (aliases can be separated by '|').
     * @param abstract A brief description for root help listings.
     * @param description Detailed help text for 'help <command>'.
     * @return A NodeBuilder focused on the root command node.
     */
    NodeBuilder Command(std::string name, std::string abstract, std::string description);

    /**
     * @brief Registers a root-level command with automatic argument parsing.
     *
     * @param name The name of the command (aliases can be separated by '|').
     * @param abstract Short description for root help listings.
     * @param handler The callable logic to execute.
     * @return A NodeBuilder focused on the registered command.
     */
    template <typename F>
    NodeBuilder On(std::string name, std::string abstract, F&& handler) {
        return On(std::move(name), std::move(abstract), "", std::forward<F>(handler));
    }

    /**
     * @brief Registers a root-level command with automatic parsing and detailed help.
     *
     * @param name The name of the command (aliases can be separated by '|').
     * @param abstract Short description for root help listings.
     * @param description Detailed help text.
     * @param handler The callable logic to execute.
     * @return A NodeBuilder focused on the registered command.
     */
    template <typename F>
    NodeBuilder On(std::string name, std::string abstract, std::string description, F&& handler) {
        return Command(std::move(name), std::move(abstract), std::move(description))
                .Handler(WrapHandler(std::forward<F>(handler)));
    }

    /**
     * @brief Finalizes the command tree and returns a CommandRegistry.
     *
     * Help commands (`help`, `h`, `?`, and `help-verbose`) are
     * automatically included in the resulting registry.
     */
    std::unique_ptr<CommandRegistry> Build();

  private:
    // Wraps a typed command handler into a generic CommandRegistry::CommandHandler.
    template <typename F>
    static CommandRegistry::CommandHandler WrapHandler(F&& handler) {
        using CtxType = typename parsing::FunctionTraits<std::decay_t<F>>::ContextType;
        return [func = std::forward<F>(handler)](
                       LineCommandHandler::Context& ctx,
                       ArgStream& args) mutable -> absl::StatusOr<std::string> {
            return parsing::BindAndInvoke(std::forward<decltype(func)>(func),
                                          static_cast<CtxType&>(ctx), args);
        };
    }

    static CommandRegistry::Entry* GetOrCreateChild(
            std::vector<std::unique_ptr<CommandRegistry::Entry>>& children, std::string name,
            std::string abstract, std::string description);

    std::filesystem::path token_path_;
    std::vector<std::unique_ptr<CommandRegistry::Entry>> root_commands_;
    CommandRegistry::CommandObserver command_observer_;
};

}  // namespace goldfish::telnet
