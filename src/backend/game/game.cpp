/*  LOOT

    A load order optimisation tool for Oblivion, Skyrim, Fallout 3 and
    Fallout: New Vegas.

    Copyright (C) 2012-2015    WrinklyNinja

    This file is part of LOOT.

    LOOT is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    LOOT is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LOOT.  If not, see
    <http://www.gnu.org/licenses/>.
    */

#include "game.h"
#include "../globals.h"
#include "../helpers/helpers.h"
#include "../error.h"

#include <thread>

#include <boost/algorithm/string.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>

using namespace std;

namespace fs = boost::filesystem;
namespace lc = boost::locale;

namespace loot {
    Game::Game() : _pluginsFullyLoaded(false) {}

    Game::Game(const GameSettings& gameSettings) : GameSettings(gameSettings), _pluginsFullyLoaded(false) {
        this->SetName(gameSettings.Name())
            .SetMaster(gameSettings.Master())
            .SetRepoURL(gameSettings.RepoURL())
            .SetRepoBranch(gameSettings.RepoBranch())
            .SetGamePath(gameSettings.GamePath())
            .SetRegistryKey(gameSettings.RegistryKey());
    }

    Game::Game(const Game& game) :
        GameSettings(game),
        LoadOrderHandler(game),
        GameCache(game),
        masterlist(game.masterlist),
        userlist(game.userlist),
        plugins(game.plugins),
        _pluginsFullyLoaded(game.ArePluginsFullyLoaded()) {}

    Game::Game(const unsigned int gameCode, const std::string& folder) : GameSettings(gameCode, folder), _pluginsFullyLoaded(false) {}

    Game& Game::operator= (const Game& game) {
        if (&game != this) {
            GameSettings::operator=(game);
            LoadOrderHandler::operator=(game);
            GameCache::operator=(game);

            masterlist = game.masterlist;
            userlist = game.userlist;
            plugins = game.plugins;
            _pluginsFullyLoaded = game.ArePluginsFullyLoaded();
        }
        return *this;
    }

    void Game::Init(bool createFolder, const boost::filesystem::path& gameLocalAppData) {
        if (Id() != Game::tes4 && Id() != Game::tes5 && Id() != Game::fo3 && Id() != Game::fonv && Id() != Game::fo4) {
            throw error(error::invalid_args, lc::translate("Invalid game ID supplied.").str());
        }

        BOOST_LOG_TRIVIAL(info) << "Initialising filesystem-related data for game: " << Name();

        if (!this->IsInstalled()) {
            BOOST_LOG_TRIVIAL(error) << "Game path could not be detected.";
            throw error(error::path_not_found, lc::translate("Game path could not be detected.").str());
        }

        if (createFolder) {
            //Make sure that the LOOT game path exists.
            try {
                if (!fs::exists(g_path_local / FolderName()))
                    fs::create_directories(g_path_local / FolderName());
            }
            catch (fs::filesystem_error& e) {
                BOOST_LOG_TRIVIAL(error) << "Could not create LOOT folder for game. Details: " << e.what();
                throw error(error::path_write_fail, lc::translate("Could not create LOOT folder for game. Details:").str() + " " + e.what());
            }
        }

        LoadOrderHandler::Init(*this, gameLocalAppData);
    }

    void Game::RedatePlugins() {
        if (Id() != tes5)
            return;

        list<string> loadorder = GetLoadOrder();
        if (!loadorder.empty()) {
            time_t lastTime = 0;
            for (const auto &pluginName : loadorder) {
                fs::path filepath = DataPath() / pluginName;
                if (!fs::exists(filepath)) {
                    if (fs::exists(filepath.string() + ".ghost"))
                        filepath += ".ghost";
                    else
                        continue;
                }

                time_t thisTime = fs::last_write_time(filepath);
                BOOST_LOG_TRIVIAL(info) << "Current timestamp for \"" << filepath.filename().string() << "\": " << thisTime;
                if (thisTime >= lastTime) {
                    lastTime = thisTime;
                    BOOST_LOG_TRIVIAL(trace) << "No need to redate \"" << filepath.filename().string() << "\".";
                }
                else {
                    lastTime += 60;
                    fs::last_write_time(filepath, lastTime);  //Space timestamps by a minute.
                    BOOST_LOG_TRIVIAL(info) << "Redated \"" << filepath.filename().string() << "\" to: " << lastTime;
                }
            }
        }
    }

    void Game::LoadPlugins(bool headersOnly) {
        uintmax_t meanFileSize = 0;
        multimap<uintmax_t, string> sizeMap;

        // First find out how many plugins there are, and their sizes.
        BOOST_LOG_TRIVIAL(trace) << "Scanning for plugins in " << this->DataPath();
        for (fs::directory_iterator it(this->DataPath()); it != fs::directory_iterator(); ++it) {
            if (fs::is_regular_file(it->status()) && Plugin::IsValid(it->path().filename().string(), *this)) {
                string name = it->path().filename().string();
                BOOST_LOG_TRIVIAL(info) << "Found plugin: " << name;

                // Trim .ghost extension if present.
                if (boost::iends_with(name, ".ghost"))
                    name = name.substr(0, name.length() - 6);

                uintmax_t fileSize = fs::file_size(it->path());
                meanFileSize += fileSize;

                sizeMap.emplace(fileSize, name);
            }
        }
        meanFileSize /= sizeMap.size();  //Rounding error, but not important.

        // Reserve space in the plugins unordered_map to speed up inserting
        // later and more importantly avoid any inserts invalidating
        // iterators.
        plugins.reserve(sizeMap.size());

        // Get the number of threads to use.
        // hardware_concurrency() may be zero, if so then use only one thread.
        size_t threadsToUse = std::min((size_t)thread::hardware_concurrency(), sizeMap.size());
        threadsToUse = std::max(threadsToUse, (size_t)1);

        // Divide the plugins up by thread.
        unsigned int pluginsPerThread = ceil((double)sizeMap.size() / threadsToUse);
        vector<vector<string>> pluginGroups(threadsToUse);
        BOOST_LOG_TRIVIAL(info) << "Loading " << sizeMap.size() << " plugins using " << threadsToUse << " threads, with up to " << pluginsPerThread << " plugins per thread.";

        // The plugins should be split between the threads so that the data
        // load is as evenly spread as possible.
        size_t currentGroup = 0;
        for (const auto& plugin : sizeMap) {
            if (currentGroup == threadsToUse)
                currentGroup = 0;
            BOOST_LOG_TRIVIAL(trace) << "Adding plugin " << plugin.second << " to loading group " << currentGroup;
            pluginGroups[currentGroup].push_back(plugin.second);
            ++currentGroup;
        }

        // Load the plugins.
        BOOST_LOG_TRIVIAL(trace) << "Starting plugin loading.";
        vector<thread> threads;
        while (threads.size() < threadsToUse) {
            vector<string>& pluginGroup = pluginGroups[threads.size()];
            threads.push_back(thread([&]() {
                for (auto pluginName : pluginGroup) {
                    BOOST_LOG_TRIVIAL(trace) << "Loading " << pluginName;
                    addPlugin(Plugin(*this, pluginName, headersOnly));
                }
            }));
        }

        // Join all threads.
        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }

        _pluginsFullyLoaded = !headersOnly;
    }

    bool Game::ArePluginsFullyLoaded() const {
        return _pluginsFullyLoaded;
    }

    bool Game::IsPluginActive(const std::string& pluginName) const {
        auto it = plugins.find(boost::locale::to_lower(pluginName));
        if (it != end(plugins))
            return it->second.IsActive();
        else
            return LoadOrderHandler::IsPluginActive(pluginName);
    }

    void Game::addPlugin(const Plugin&& plugin) {
        std::lock_guard<std::mutex> lock(mutex);
        plugins.emplace(boost::locale::to_lower(plugin.Name()), plugin);
    }

    std::list<Game> ToGames(const std::list<GameSettings>& settings) {
        return list<Game>(settings.begin(), settings.end());
    }

    std::list<GameSettings> ToGameSettings(const std::list<Game>& games) {
        return list<GameSettings>(games.begin(), games.end());
    }
}
