#include "AgentMessage.h"

#include <boost/json.hpp>

#include <format>

namespace YirangAgent
{
	auto parse_agent_message(const std::string& text) -> std::expected<AgentMessage, std::string>
	{
		boost::json::value parsed;
		try
		{
			parsed = boost::json::parse(text);
		}
		catch (const std::exception& exception)
		{
			return std::unexpected(std::format("cannot parse agent message: {}", exception.what()));
		}

		if (!parsed.is_object())
		{
			return std::unexpected("agent message root is not a JSON object");
		}

		const auto root = parsed.as_object();

		if (!root.contains("command") || !root.at("command").is_string())
		{
			return std::unexpected("agent message field 'command' is missing or not a string");
		}

		AgentMessage message;
		message.command = root.at("command").as_string().c_str();

		if (message.command.empty())
		{
			return std::unexpected("agent message field 'command' is empty");
		}

		if (root.contains("payload"))
		{
			const auto& payload = root.at("payload");
			message.payload = payload.is_string() ? std::string(payload.as_string().c_str()) : boost::json::serialize(payload);
		}
		else
		{
			message.payload = "{}";
		}

		if (root.contains("reply_queue_url") && root.at("reply_queue_url").is_string())
		{
			message.reply_queue_url = root.at("reply_queue_url").as_string().c_str();
		}

		return message;
	}

	auto serialize_agent_message(const AgentMessage& message) -> std::string
	{
		boost::json::object root;
		root["command"] = message.command;

		boost::json::value payload;
		try
		{
			payload = boost::json::parse(message.payload);
		}
		catch (const std::exception&)
		{
			payload = boost::json::value(message.payload);
		}
		root["payload"] = payload;

		if (!message.reply_queue_url.empty())
		{
			root["reply_queue_url"] = message.reply_queue_url;
		}

		return boost::json::serialize(root);
	}
}
