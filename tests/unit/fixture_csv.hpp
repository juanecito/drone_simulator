// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Minimal reader for the CSV reference tables in tests/fixtures (comment lines start with '#',
// the first other line is the header, no quoting).
namespace ds::test {

inline std::filesystem::path fixturePath(std::string_view relative) {
    return std::filesystem::path(DS_TEST_FIXTURES_DIR) / relative;
}

class CsvTable {
public:
    explicit CsvTable(std::string_view relative) {
        std::ifstream in(fixturePath(relative));
        if (!in) {
            throw std::runtime_error("cannot open fixture " + fixturePath(relative).string());
        }
        std::string line;
        bool header = true;
        while (std::getline(in, line)) {
            if (line.empty() || line.front() == '#') {
                continue;
            }
            auto fields = split(line);
            if (header) {
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    columns_[fields[i]] = i;
                }
                header = false;
            } else {
                rows_.push_back(std::move(fields));
            }
        }
    }

    [[nodiscard]] std::size_t size() const { return rows_.size(); }

    [[nodiscard]] const std::string& text(std::size_t row, const std::string& column) const {
        return rows_.at(row).at(columns_.at(column));
    }

    [[nodiscard]] double number(std::size_t row, const std::string& column) const {
        const std::string& field = text(row, column);
        double value = 0.0;
        const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (ec != std::errc{} || ptr != field.data() + field.size()) {
            throw std::runtime_error("not a number: '" + field + "' in column " + column);
        }
        return value;
    }

private:
    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto comma = line.find(',', start);
            fields.push_back(line.substr(start, comma - start));
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return fields;
    }

    std::map<std::string, std::size_t> columns_;
    std::vector<std::vector<std::string>> rows_;
};

}  // namespace ds::test
