/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * IULIA interpreter.
 */

#include <libjulia/interpreter/Interpreter.h>

#include <libjulia/interpreter/EVMInstructionInterpreter.h>

#include <libsolidity/inlineasm/AsmData.h>

#include <libsolidity/interface/Exceptions.h>

#include <libdevcore/FixedHash.h>

#include <boost/range/adaptor/reversed.hpp>

using namespace std;
using namespace dev;
using namespace dev::julia;


void Interpreter::operator()(ExpressionStatement const& _expressionStatement)
{
	evaluateMulti(_expressionStatement.expression);
}

void Interpreter::operator()(Assignment const& _assignment)
{
	solAssert(_assignment.value, "");
	vector<u256> values = evaluateMulti(*_assignment.value);
	solAssert(values.size() == _assignment.variableNames.size(), "");
	for (size_t i = 0; i < values.size(); ++i)
	{
		string const& varName = _assignment.variableNames.at(i).name;
		solAssert(m_variables.count(varName), "");
		m_variables[varName] = values.at(i);
	}
}

void Interpreter::operator()(VariableDeclaration const& _declaration)
{
	vector<u256> values(_declaration.variables.size(), 0);
	if (_declaration.value)
		values = evaluateMulti(*_declaration.value);

	solAssert(values.size() == _declaration.variables.size(), "");
	for (size_t i = 0; i < values.size(); ++i)
	{
		string const& varName = _declaration.variables.at(i).name;
		solAssert(!m_variables.count(varName), "");
		m_variables[varName] = values.at(i);
	}
}

void Interpreter::operator()(If const& _if)
{
	solAssert(_if.condition, "");
	if (evaluate(*_if.condition) != 0)
		(*this)(_if.body);
}

void Interpreter::operator()(Switch const& _switch)
{
	solAssert(_switch.expression, "");
	u256 val = evaluate(*_switch.expression);
	for (auto const& c: _switch.cases)
		// Default case has to be last.
		if (!c.value || evaluate(*c.value) == val)
		{
			(*this)(c.body);
			break;
		}
}

void Interpreter::operator()(FunctionDefinition const&)
{
}

void Interpreter::operator()(ForLoop const& _forLoop)
{
	solAssert(_forLoop.condition, "");

	openScope();
	for (auto const& statement: _forLoop.pre.statements)
		visit(statement);
	while (evaluate(*_forLoop.condition) != 0)
	{
		(*this)(_forLoop.body);
		(*this)(_forLoop.post);
	}
	closeScope();
}

void Interpreter::operator()(Block const& _block)
{
	openScope();
	// Register functions.
	for (auto const& statement: _block.statements)
		if (statement.type() == typeid(FunctionDefinition))
		{
			FunctionDefinition const& funDef = boost::get<FunctionDefinition>(statement);
			m_functions[funDef.name] = &funDef;
		}
	ASTWalker::operator()(_block);
	closeScope();
}

u256 Interpreter::evaluate(Expression const& _expression)
{
	ExpressionEvaluator ev(m_state, m_variables, m_functions);
	ev.visit(_expression);
	return ev.value();
}

vector<u256> Interpreter::evaluateMulti(Expression const& _expression)
{
	ExpressionEvaluator ev(m_state, m_variables, m_functions);
	ev.visit(_expression);
	return ev.values();
}

void Interpreter::closeScope()
{
	for (auto const& var: m_scopes.back())
	{
		solAssert(m_variables.erase(var) + m_functions.erase(var) == 1, "");
	}
}

void ExpressionEvaluator::operator()(Literal const& _literal)
{
	using solidity::assembly::LiteralKind;
	switch (_literal.kind)
	{
	case LiteralKind::Boolean:
		solAssert(_literal.value == "true" || _literal.value == "false", "");
		setValue(_literal.value == "true" ? 1 : 0);
		break;
	case LiteralKind::Number:
		setValue(u256(_literal.value));
		break;
	case LiteralKind::String:
		solAssert(_literal.value.size() <= 32, "");
		setValue(u256(h256(_literal.value, h256::FromBinary, h256::AlignLeft)));
		break;
	}
}

void ExpressionEvaluator::operator()(Identifier const& _identifier)
{
	solAssert(m_variables.count(_identifier.name), "");
	setValue(m_variables.at(_identifier.name));
}

void ExpressionEvaluator::operator()(FunctionalInstruction const& _instr)
{
	evaluateArgs(_instr.arguments);
	EVMInstructionInterpreter interpreter(m_state);
	// The instruction might also return nothing, but it does not
	// hurt to set the value in that case.
	setValue(interpreter.eval(_instr.instruction, values()));
}

void ExpressionEvaluator::operator()(FunctionCall const& _funCall)
{
	solAssert(m_functions.count(_funCall.functionName.name), "");
	evaluateArgs(_funCall.arguments);

	FunctionDefinition const& fun = *m_functions.at(_funCall.functionName.name);
	solAssert(m_values.size() == fun.parameters.size(), "");
	map<string, u256> arguments;
	for (size_t i = 0; i < fun.parameters.size(); ++i)
		arguments[fun.parameters.at(i).name] = m_values.at(i);
	for (size_t i = 0; i < fun.returnVariables.size(); ++i)
		arguments[fun.returnVariables.at(i).name] = 0;

	// TODO function name lookup could be a little more efficient,
	// we have to copy the list here.
	Interpreter interpreter(m_state, arguments, m_functions);
	interpreter(fun.body);

	m_values.clear();
	for (auto const& retVar: fun.returnVariables)
		m_values.emplace_back(interpreter.valueOfVariable(retVar.name));
}

void ExpressionEvaluator::evaluateArgs(vector<Expression> const& _expr)
{
	vector<u256> values;
	for (auto const& expr: _expr | boost::adaptors::reversed)
	{
		visit(expr);
		values.push_back(value());
	}
	m_values = std::move(values);
	std::reverse(m_values.begin(), m_values.end());
}
