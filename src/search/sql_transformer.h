/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#pragma once

#include <limits>
#include <memory>
#include <variant>

#include "common_transformer.h"
#include "ir.h"
#include "parse_util.h"
#include "sql_parser.h"

namespace kqir {

namespace sql {

namespace ir = kqir;

template <typename Rule>
using TreeSelector = parse_tree::selector<
    Rule,
    parse_tree::store_content::on<Boolean, Number, String, Identifier, NumericCompareOp, AscOrDesc, UnsignedInteger>,
    parse_tree::remove_content::on<HasTagExpr, NumericCompareExpr, NotExpr, AndExpr, OrExpr, Wildcard, SelectExpr,
                                   FromExpr, WhereClause, OrderByClause, LimitClause, SearchStmt>>;

template <typename Input>
StatusOr<std::unique_ptr<parse_tree::node>> ParseToTree(Input&& in) {
  if (auto root = parse_tree::parse<seq<SearchStmt, eof>, TreeSelector>(std::forward<Input>(in))) {
    return root;
  } else {
    // TODO: improve parse error message, with source location
    return {Status::NotOK, "invalid syntax"};
  }
}

struct Transformer : ir::TreeTransformer {
  static auto Transform(const TreeNode& node) -> StatusOr<std::unique_ptr<Node>> {
    if (Is<Boolean>(node)) {
      return Node::Create<ir::BoolLiteral>(node->string_view() == "true");
    } else if (Is<Number>(node)) {
      return Node::Create<ir::NumericLiteral>(*ParseFloat(node->string()));
    } else if (Is<String>(node)) {
      return Node::Create<ir::StringLiteral>(GET_OR_RET(UnescapeString(node->string_view())));
    } else if (Is<HasTagExpr>(node)) {
      CHECK(node->children.size() == 2);

      return Node::Create<ir::TagContainExpr>(
          std::make_unique<ir::FieldRef>(node->children[0]->string()),
          Node::MustAs<ir::StringLiteral>(GET_OR_RET(Transform(node->children[1]))));
    } else if (Is<NumericCompareExpr>(node)) {
      CHECK(node->children.size() == 3);

      const auto& lhs = node->children[0];
      const auto& rhs = node->children[2];

      auto op = ir::NumericCompareExpr::FromOperator(node->children[1]->string_view()).value();
      if (Is<Identifier>(lhs) && Is<Number>(rhs)) {
        return Node::Create<ir::NumericCompareExpr>(op, std::make_unique<ir::FieldRef>(lhs->string()),
                                                    Node::MustAs<ir::NumericLiteral>(GET_OR_RET(Transform(rhs))));
      } else if (Is<Number>(lhs) && Is<Identifier>(rhs)) {
        return Node::Create<ir::NumericCompareExpr>(ir::NumericCompareExpr::Flip(op),
                                                    std::make_unique<ir::FieldRef>(rhs->string()),
                                                    Node::MustAs<ir::NumericLiteral>(GET_OR_RET(Transform(lhs))));
      } else {
        return {Status::NotOK, "the left and right side of numeric comparison should be an identifier and a number"};
      }
    } else if (Is<NotExpr>(node)) {
      CHECK(node->children.size() == 1);

      return Node::Create<ir::NotExpr>(Node::MustAs<ir::QueryExpr>(GET_OR_RET(Transform(node->children[0]))));
    } else if (Is<AndExpr>(node)) {
      std::vector<std::unique_ptr<ir::QueryExpr>> exprs;

      for (const auto& child : node->children) {
        exprs.push_back(Node::MustAs<ir::QueryExpr>(GET_OR_RET(Transform(child))));
      }

      return Node::Create<ir::AndExpr>(std::move(exprs));
    } else if (Is<OrExpr>(node)) {
      std::vector<std::unique_ptr<ir::QueryExpr>> exprs;

      for (const auto& child : node->children) {
        exprs.push_back(Node::MustAs<ir::QueryExpr>(GET_OR_RET(Transform(child))));
      }

      return Node::Create<ir::OrExpr>(std::move(exprs));
    } else if (Is<SelectExpr>(node)) {
      std::vector<std::unique_ptr<ir::FieldRef>> fields;

      if (node->children.size() == 1 && Is<Wildcard>(node->children[0])) {
        return Node::Create<ir::SelectExpr>(std::move(fields));
      }

      for (const auto& child : node->children) {
        fields.push_back(std::make_unique<ir::FieldRef>(child->string()));
      }

      return Node::Create<ir::SelectExpr>(std::move(fields));
    } else if (Is<FromExpr>(node)) {
      CHECK(node->children.size() == 1);
      return Node::Create<ir::IndexRef>(node->children[0]->string());
    } else if (Is<WhereClause>(node)) {
      CHECK(node->children.size() == 1);
      return Transform(node->children[0]);
    } else if (Is<LimitClause>(node)) {
      CHECK(node->children.size() == 1 || node->children.size() == 2);

      size_t offset = 0, count = std::numeric_limits<size_t>::max();
      if (node->children.size() == 1) {
        count = *ParseInt(node->children[0]->string());
      } else {
        offset = *ParseInt(node->children[0]->string());
        count = *ParseInt(node->children[1]->string());
      }

      return Node::Create<ir::Limit>(offset, count);
    }
    if (Is<OrderByClause>(node)) {
      CHECK(node->children.size() == 1 || node->children.size() == 2);

      auto field = std::make_unique<FieldRef>(node->children[0]->string());
      auto order = SortBy::Order::ASC;
      if (node->children.size() == 2 && node->children[1]->string_view() == "desc") {
        order = SortBy::Order::DESC;
      }

      return Node::Create<SortBy>(order, std::move(field));
    } else if (Is<SearchStmt>(node)) {  // root node
      CHECK(node->children.size() >= 2 && node->children.size() <= 5);

      auto index = Node::MustAs<ir::IndexRef>(GET_OR_RET(Transform(node->children[1])));
      auto select = Node::MustAs<ir::SelectExpr>(GET_OR_RET(Transform(node->children[0])));

      std::unique_ptr<ir::QueryExpr> query_expr;
      std::unique_ptr<ir::Limit> limit;
      std::unique_ptr<ir::SortBy> sort_by;

      for (size_t i = 2; i < node->children.size(); ++i) {
        if (Is<WhereClause>(node->children[i])) {
          query_expr = Node::MustAs<ir::QueryExpr>(GET_OR_RET(Transform(node->children[i])));
        } else if (Is<LimitClause>(node->children[i])) {
          limit = Node::MustAs<ir::Limit>(GET_OR_RET(Transform(node->children[i])));
        } else if (Is<OrderByClause>(node->children[i])) {
          sort_by = Node::MustAs<ir::SortBy>(GET_OR_RET(Transform(node->children[i])));
        }
      }

      return Node::Create<ir::SearchStmt>(std::move(index), std::move(query_expr), std::move(limit), std::move(sort_by),
                                          std::move(select));
    } else if (IsRoot(node)) {
      CHECK(node->children.size() == 1);

      return Transform(node->children[0]);
    } else {
      // UNREACHABLE CODE, just for debugging here
      return {Status::NotOK, fmt::format("encountered invalid node type: {}", node->type)};
    }
  }
};

template <typename Input>
StatusOr<std::unique_ptr<ir::Node>> ParseToIR(Input&& in) {
  return Transformer::Transform(GET_OR_RET(ParseToTree(std::forward<Input>(in))));
}

}  // namespace sql

}  // namespace kqir
