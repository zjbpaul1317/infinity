//
// Created by JinHai on 2022/9/14.
//

#pragma once
#include "function.h"
#include "storage/data_type.h"

namespace infinity {
class ScalarFunction : public Function {
public:
    explicit ScalarFunction(std::string name,
                            const std::vector<LogicalType>& argument_types,
                            LogicalType& return_type);

private:
    std::vector<LogicalType> argument_types_;
    LogicalType return_type_;

};
}

