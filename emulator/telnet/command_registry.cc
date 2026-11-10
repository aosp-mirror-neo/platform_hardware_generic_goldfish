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
#include "command_registry.h"

#include <algorithm>
#include <vector>

#include "absl/log/check.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"

namespace goldfish::telnet {
namespace {

// Appends token to target separated by a space if target is non-empty.
void AppendSpaceSeparated(std::string* target, std::string_view token) {
    if (!target->empty()) {
        *target += " ";
    }
    target->append(token.data(), token.size());
}

// Returns true if the command entry is safe or the session is authenticated.
bool IsAccessible(const CommandRegistry::Entry& entry, const LineCommandHandler::Context& ctx) {
    return entry.is_safe || ctx.authenticated;
}

}  // namespace

// --- Static Helpers ---

// Checks if the entry name or any of its aliases match the provided name.
bool CommandRegistry::Matches(const Entry& entry, std::string_view name) {
    if (name.empty()) return false;

    std::string_view s = entry.name;
    while (true) {
        const size_t pos = s.find(name);
        if (pos == std::string_view::npos) return false;

        const bool prefix_ok = (pos == 0 || s[pos - 1] == '|');
        const bool suffix_ok = (pos + name.size() == s.size() || s[pos + name.size()] == '|');

        if (prefix_ok && suffix_ok) return true;

        s.remove_prefix(pos + 1);
    }
}

std::string_view CommandRegistry::GetCanonicalName(std::string_view names) {
    const size_t pos = names.find('|');
    return pos == std::string_view::npos ? names : names.substr(0, pos);
}

// Finds the first child in the list that matches the given name.
const CommandRegistry::Entry* CommandRegistry::FindChild(
        const std::vector<std::unique_ptr<Entry>>& children, std::string_view name) {
    for (const auto& child : children) {
        if (Matches(*child, name)) {
            return child.get();
        }
    }
    return nullptr;
}

// Finds an existing child or creates a new one with the given name and description.
CommandRegistry::Entry* CommandRegistryBuilder::GetOrCreateChild(
        std::vector<std::unique_ptr<CommandRegistry::Entry>>& children, std::string name,
        std::string abstract, std::string description) {
    for (const auto& child : children) {
        if (CommandRegistry::Matches(*child, name)) {
            if (!abstract.empty()) child->abstract = std::move(abstract);
            if (!description.empty()) child->description = std::move(description);
            return child.get();
        }
    }
    children.push_back(std::make_unique<CommandRegistry::Entry>(
            CommandRegistry::Entry{.name = std::move(name),
                                   .abstract = std::move(abstract),
                                   .description = std::move(description)}));
    return children.back().get();
}

// --- NodeBuilder implementation ---

CommandRegistryBuilder::NodeBuilder::NodeBuilder(CommandRegistry::Entry* entry) : entry_(entry) {}

CommandRegistryBuilder::NodeBuilder& CommandRegistryBuilder::NodeBuilder::Safe() {
    entry_->is_safe = true;
    return *this;
}

CommandRegistryBuilder::NodeBuilder& CommandRegistryBuilder::NodeBuilder::Handler(
        CommandRegistry::CommandHandler handler) {
    entry_->handler = std::move(handler);
    return *this;
}

CommandRegistryBuilder::NodeBuilder& CommandRegistryBuilder::NodeBuilder::OnInternal(
        std::string name, std::string abstract, std::string description,
        CommandRegistry::CommandHandler handler) {
    Sub(std::move(name), std::move(abstract), std::move(description)).Handler(std::move(handler));
    return *this;
}

CommandRegistryBuilder::NodeBuilder CommandRegistryBuilder::NodeBuilder::Sub(std::string name,
                                                                             std::string abstract) {
    return Sub(std::move(name), std::move(abstract), "");
}

CommandRegistryBuilder::NodeBuilder CommandRegistryBuilder::NodeBuilder::Sub(
        std::string name, std::string abstract, std::string description) {
    DCHECK(!name.empty()) << "Sub-command name cannot be empty.";
    CommandRegistry::Entry* child = CommandRegistryBuilder::GetOrCreateChild(
            entry_->children, std::move(name), std::move(abstract), std::move(description));
    if (entry_->is_safe) {
        child->is_safe = true;
    }
    return NodeBuilder{child};
}

// --- Builder implementation ---

CommandRegistryBuilder::CommandRegistryBuilder(std::filesystem::path token_path,
                                               CommandRegistry::CommandObserver command_observer)
        : token_path_(std::move(token_path)), command_observer_(std::move(command_observer)) {}

CommandRegistryBuilder::NodeBuilder CommandRegistryBuilder::Command(std::string name,
                                                                    std::string abstract) {
    return Command(std::move(name), std::move(abstract), "");
}

CommandRegistryBuilder::NodeBuilder CommandRegistryBuilder::Command(std::string name,
                                                                    std::string abstract,
                                                                    std::string description) {
    CommandRegistry::Entry* entry = GetOrCreateChild(root_commands_, std::move(name),
                                                     std::move(abstract), std::move(description));
    return NodeBuilder{entry};
}

std::unique_ptr<CommandRegistry> CommandRegistryBuilder::Build() {
    return std::make_unique<CommandRegistry>(CommandRegistry::Passkey{}, std::move(token_path_),
                                             std::move(root_commands_),
                                             std::move(command_observer_));
}

// --- Registry implementation ---

CommandRegistry::CommandRegistry(Passkey, std::filesystem::path token_path,
                                 std::vector<std::unique_ptr<Entry>> root_commands,
                                 CommandObserver command_observer)
        : token_path_(std::move(token_path)), command_observer_(std::move(command_observer)) {
    // Register built-in help commands at the beginning to match old behavior.
    auto help_verbose = std::make_unique<Entry>(Entry{
        .name = "help-verbose",
        .abstract = "print a list of commands with descriptions",
        .handler = [this](Context& ctx, ArgStream& args) { return ShowHelp(args, ctx, true); },
        .is_safe = true});

    auto help = std::make_unique<Entry>(Entry{
        .name = "help|h|?",
        .abstract = "print a list of commands",
        .handler = [this](Context& ctx, ArgStream& args) { return ShowHelp(args, ctx, false); },
        .is_safe = true});

    root_commands_.reserve(root_commands.size() + 2);
    root_commands_.push_back(std::move(help));
    root_commands_.push_back(std::move(help_verbose));
    for (auto& entry : root_commands) {
        root_commands_.push_back(std::move(entry));
    }
}

absl::StatusOr<std::string> CommandRegistry::operator()(std::string line, Context& ctx) {
    ArgStream args{std::move(line)};
    return DispatchCommand(args, ctx);
}

// Traverses the command tree based on input arguments and executes the matched handler.
absl::StatusOr<std::string> CommandRegistry::DispatchCommand(ArgStream& args, Context& ctx) const {
    const Entry* current = nullptr;
    const std::vector<std::unique_ptr<Entry>>* children = &root_commands_;
    std::string user_path;       // Accumulated user-typed command path for help/error display.
    std::string canonical_path;  // Accumulated canonical command path for metrics tracking.
    std::string_view top_level_name;

    while (!args.Empty()) {
        const std::string name = args.Peek();
        const Entry* next = FindChild(*children, name);
        if (!next) break;

        args.Next();
        const std::string_view canonical_name = GetCanonicalName(next->name);
        if (!current) {
            top_level_name = canonical_name;
        }
        current = next;
        children = &current->children;

        AppendSpaceSeparated(&user_path, name);
        AppendSpaceSeparated(&canonical_path, canonical_name);

        if (!IsAccessible(*current, ctx)) {
            // Do not leak existence of commands that require authentication.
            return absl::NotFoundError("unknown command, try 'help'.");
        }

        if (current->handler) {
            if (command_observer_) {
                command_observer_(top_level_name, canonical_path);
            }
            return current->handler(ctx, args);
        }
    }

    if (current) {
        return absl::InvalidArgumentError(absl::StrCat(
                "missing sub-command\r\n", GetSubCommandsHelp(*current, user_path, ctx)));
    }

    return absl::NotFoundError("unknown command, try 'help'.");
}

// Dispatches to the appropriate help generator based on whether arguments are present.
absl::StatusOr<std::string> CommandRegistry::ShowHelp(ArgStream& args, Context& ctx,
                                                      bool verbose) const {
    if (args.Empty()) {
        return ShowRootHelp(ctx, verbose);
    }
    return ShowSubcommandHelp(args, ctx);
}

// Generates the top-level help message listing all available root commands.
std::string CommandRegistry::ShowRootHelp(const Context& ctx, bool verbose) const {
    std::string help =
            verbose ? "Android console command help:\r\n\r\n" : "Android console commands:\r\n";
    for (const auto& cmd : root_commands_) {
        if (IsAccessible(*cmd, ctx)) {
            if (verbose) {
                absl::StrAppend(&help,
                                absl::StrFormat("    %-16s %s\r\n", cmd->name, cmd->abstract));
            } else {
                absl::StrAppend(&help, "    ", cmd->name, "\r\n");
            }
        }
    }

    if (verbose) {
        absl::StrAppend(&help, "\ntry 'help <command>' for command-specific help");
    } else {
        absl::StrAppend(&help,
                        "\r\nTry 'help-verbose' for more description\r\n"
                        "Try 'help <command>' for command-specific help");
    }
    return help;
}

// Traverses the tree to find a specific subcommand and displays its help.
absl::StatusOr<std::string> CommandRegistry::ShowSubcommandHelp(ArgStream& args,
                                                                const Context& ctx) const {
    const Entry* current = nullptr;
    const std::vector<std::unique_ptr<Entry>>* children = &root_commands_;
    std::string path;

    while (!args.Empty()) {
        const std::string next_name = args.Next();
        current = FindChild(*children, next_name);
        if (!current || !IsAccessible(*current, ctx)) {
            return absl::NotFoundError("unknown command, try 'help'.");
        }
        AppendSpaceSeparated(&path, next_name);
        children = &current->children;
    }

    return GetSubCommandsHelp(*current, path, ctx, true);
}

// Formats the help message for a specific command node and its subcommands.
std::string CommandRegistry::GetSubCommandsHelp(const Entry& entry, std::string_view path,
                                                const Context& ctx, bool verbose) {
    std::string help;
    if (!entry.description.empty()) {
        help = absl::StrCat(entry.description, "\r\n");
    } else {
        help = absl::StrCat(path, "\r\n", entry.abstract, "\r\n");
    }

    bool has_subs = false;
    for (const auto& child : entry.children) {
        if (IsAccessible(*child, ctx)) {
            if (!has_subs) {
                absl::StrAppend(&help, "\navailable sub-commands:\r\n");
                has_subs = true;
            }
            const std::string full_name =
                    path.empty() ? child->name : absl::StrCat(path, " ", child->name);
            if (verbose) {
                absl::StrAppend(&help,
                                absl::StrFormat("    %-20s %s\r\n", full_name, child->abstract));
            } else {
                absl::StrAppend(&help, "    ", full_name, "\r\n");
            }
        }
    }
    return help;
}

std::string CommandRegistry::WelcomeMessage(const Context& ctx) const {
    if (!ctx.authenticated) {
        return absl::StrCat(
                "Android Console: Authentication required\r\n"
                "Android Console: type 'auth <auth_token>' to authenticate\r\n"
                "Android Console: you can find your <auth_token> in \r\n'",
                token_path_.string(), "'\r\nOK");
    }
    return "Android Console: type 'help' for a list of commands\r\n";
}

}  // namespace goldfish::telnet
