#include <std_include.hpp>
#include "loader/module_loader.hpp"
#include "party.hpp"

#include "command.hpp"
#include "network.hpp"

#include "utils/hook.hpp"
#include "utils/cryptography.hpp"

namespace party
{
	namespace
	{
		struct
		{
			game::netadr_s host{};
			std::string challenge{};
		} connect_state;

		void connect_to_party(const game::netadr_s& target, const std::string& mapname, const std::string& gametype)
		{
			if (game::environment::is_sp())
			{
				return;
			}

			// This fixes several crashes and impure client stuff
			game::Cmd_ExecuteSingleCommand(0, 0, "xblive_privatematch 1\n");

			// CL_ConnectFromParty
			char session_info[0x100] = {};
			reinterpret_cast<void(*)(int, char*, const game::netadr_s*, const char*, const char*)>(0x1402C5700)(
				0, session_info, &target, mapname.data(), gametype.data());
		}

		void load_new_map_stub(const char* map, const char* gametype)
		{
			connect_to_party(*reinterpret_cast<game::netadr_s*>(0x141CB535C), map, gametype);
		}
	}

	void connect(const game::netadr_s& target)
	{
		if (game::environment::is_sp())
		{
			return;
		}

		connect_state.host = target;
		connect_state.challenge = utils::cryptography::random::get_challenge();

		network::send(target, "preConnect", connect_state.challenge);
	}

	class module final : public module_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp())
			{
				return;
			}

			// Hook CL_SetupForNewServerMap
			// The server seems to kick us after a map change
			// This fix is pretty bad, but it works for now
			utils::hook::jump(0x1402C9F60, &load_new_map_stub);

			command::add("map", [](command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				game::SV_StartMap(0, argument[1], false);
			});

			command::add("connect", [](command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				game::netadr_s target{};
				if (game::NET_StringToAdr(argument[1], &target))
				{
					connect(target);
				}
			});

			network::on("preConnect", [](const game::netadr_s& target, const std::string_view& data)
			{
				proto::network::connect_info info;
				info.set_valid(true);
				info.set_challenge(data.data(), data.size());

				auto* gametype = game::Dvar_FindVar("g_gametype");
				if (!gametype || !gametype->current.string)
				{
					info.set_valid(false);
				}
				else
				{
					info.set_gametype(gametype->current.string);
				}

				auto* mapname = game::Dvar_FindVar("mapname");
				if (!mapname || !mapname->current.string)
				{
					info.set_valid(false);
				}
				else
				{
					info.set_mapname(mapname->current.string);
				}

				network::send(target, "preConnectResponse", info.SerializeAsString());
			});

			network::on("preConnectResponse", [](const game::netadr_s& target, const std::string_view& data)
			{
				if (connect_state.host != target)
				{
					printf("Connect response from stray host.\n");
					return;
				}

				proto::network::connect_info info;
				if (!info.ParseFromArray(data.data(), static_cast<int>(data.size())))
				{
					printf("Unable to read connect response data.\n");
					return;
				}

				if (!info.valid())
				{
					printf("Invalid connect response data.\n");
					return;
				}

				if (info.challenge() != connect_state.challenge)
				{
					printf("Invalid challenge.\n");
					return;
				}

				connect_to_party(target, info.mapname(), info.gametype());
			});
		}
	};
}

REGISTER_MODULE(party::module)
