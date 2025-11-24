#pragma once

#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct CppMatrix {
    std::size_t rows{};
    std::size_t cols{};
    std::vector<double> data;

    double &operator()(std::size_t r, std::size_t c) {
        return data[r * cols + c];
    }

    const double &operator()(std::size_t r, std::size_t c) const {
        return data[r * cols + c];
    }
};

inline CppMatrix make_random_matrix(std::size_t rows, std::size_t cols) {
    if (rows == 0 || cols == 0) {
        throw std::invalid_argument("Matrix dimensions must be positive");
    }

    CppMatrix m;
    m.rows = rows;
    m.cols = cols;
    m.data.resize(rows * cols);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(-100.0, 100.0);

    for (auto &value : m.data) {
        value = dist(gen);
    }

    return m;
}

inline std::string serialize_matrix(const CppMatrix &m) {
    std::ostringstream oss;
    oss << m.rows << ' ' << m.cols;
    for (double value : m.data) {
        oss << ' ' << value;
    }
    oss << '\n';
    return oss.str();
}

inline CppMatrix deserialize_matrix(const std::string &payload) {
    std::istringstream iss(payload);
    CppMatrix m;
    if (!(iss >> m.rows >> m.cols)) {
        throw std::runtime_error("Invalid matrix header");
    }

    if (m.rows == 0 || m.cols == 0) {
        throw std::runtime_error("Matrix dimensions must be positive");
    }

    m.data.resize(m.rows * m.cols);
    for (double &value : m.data) {
        if (!(iss >> value)) {
            throw std::runtime_error("Unexpected end of matrix data");
        }
    }

    return m;
}
