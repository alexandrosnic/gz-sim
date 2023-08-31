/*
 * Copyright (C) 2018 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <numeric>

#ifdef HAVE_PYBIND11
#include <pybind11/embed.h>
#endif

#include <gz/common/SystemPaths.hh>
#include <gz/fuel_tools/Interface.hh>
#include <gz/fuel_tools/ClientConfig.hh>
#include <sdf/Root.hh>
#include <sdf/Error.hh>

#include "gz/sim/config.hh"
#include "gz/sim/Server.hh"
#include "gz/sim/Util.hh"

#include "ServerPrivate.hh"
#include "SimulationRunner.hh"

using namespace gz;
using namespace sim;

/// \brief This struct provides access to the default world.
struct DefaultWorld
{
  /// \brief Get the default world as a string.
  /// Plugins will be loaded from the server.config file.
  /// \return An SDF string that contains the default world.
  public: static std::string &World()
  {
    static std::string world = std::string("<?xml version='1.0'?>"
      "<sdf version='1.6'>"
        "<world name='default'>") +
        "</world>"
      "</sdf>";

    return world;
  }
};

/////////////////////////////////////////////////
Server::Server(const ServerConfig &_config)
  : dataPtr(new ServerPrivate)
{
#ifdef HAVE_PYBIND11
  if (Py_IsInitialized() == 0)
  {
    // We can't used pybind11::scoped_interpreter because:
    //   1. It gets destructed before plugins are unloaded, which can cause
    //      segfaults if the plugin tries to run python code, e.g. a message
    //      that arrives during destruction.
    //   2. It will prevent instantiation of other Servers. Running python
    //      systems will not be supported with multiple servers in the same
    //      process, but we shouldn't break existing behior for non-python use
    //      cases.
    // This means, we will not be calling pybind11::finalize_interpreter to
    // clean up the interpreter. This could cause issues with tests suites that
    // have multiple tests that load python systems.
    pybind11::initialize_interpreter();
  }
#endif

  this->dataPtr->config = _config;

  // Configure the fuel client
  fuel_tools::ClientConfig config;
  if (!_config.ResourceCache().empty())
    config.SetCacheLocation(_config.ResourceCache());
  this->dataPtr->fuelClient = std::make_unique<fuel_tools::FuelClient>(config);

  // Configure SDF to fetch assets from Gazebo Fuel.
  sdf::setFindCallback(std::bind(&ServerPrivate::FetchResource,
        this->dataPtr.get(), std::placeholders::_1));
  common::addFindFileURICallback(std::bind(&ServerPrivate::FetchResourceUri,
      this->dataPtr.get(), std::placeholders::_1));

  addResourcePaths();

  sdf::Errors errors;

  switch (_config.Source())
  {
    // Load a world if specified. Check SDF string first, then SDF file
    case ServerConfig::SourceType::kSdfRoot:
    {
      this->dataPtr->sdfRoot = _config.SdfRoot()->Clone();
      gzmsg << "Loading SDF world from SDF DOM.\n";
      break;
    }

    case ServerConfig::SourceType::kSdfString:
    {
      std::string msg = "Loading SDF string. ";
      if (_config.SdfFile().empty())
      {
        msg += "File path not available.\n";
      }
      else
      {
        msg += "File path [" + _config.SdfFile() + "].\n";
      }
      gzmsg <<  msg;
      errors = this->dataPtr->sdfRoot.LoadSdfString(_config.SdfString());
      break;
    }

    case ServerConfig::SourceType::kSdfFile:
    {
      std::string filePath = resolveSdfWorldFile(_config.SdfFile(),
          _config.ResourceCache());

      if (filePath.empty())
      {
        gzerr << "Failed to find world [" << _config.SdfFile() << "]"
               << std::endl;
        return;
      }

      gzmsg << "Loading SDF world file[" << filePath << "].\n";

      sdf::Root sdfRoot;
      // \todo(nkoenig) Async resource download.
      // This call can block for a long period of time while
      // resources are downloaded. Blocking here causes the GUI to block with
      // a black screen (search for "Async resource download" in
      // 'src/gui_main.cc'.
      errors = sdfRoot.Load(filePath);
      if (errors.empty()) {
        if (sdfRoot.Model() == nullptr) {
          this->dataPtr->sdfRoot = std::move(sdfRoot);
        }
        else
        {
          // If the specified file only contains a model, load the default
          // world and add the model to it.
          errors = this->dataPtr->sdfRoot.LoadSdfString(DefaultWorld::World());
          sdf::World *world = this->dataPtr->sdfRoot.WorldByIndex(0);
          if (world == nullptr) {
            return;
          }
          world->AddModel(*sdfRoot.Model());
          if (errors.empty()) {
            errors = this->dataPtr->sdfRoot.UpdateGraphs();
          }
        }
      }
      break;
    }

    case ServerConfig::SourceType::kNone:
    default:
    {
      gzmsg << "Loading default world.\n";
      // Load an empty world.
      /// \todo(nkoenig) Add a "AddWorld" function to sdf::Root.
      errors = this->dataPtr->sdfRoot.LoadSdfString(DefaultWorld::World());
      break;
    }
  }

  if (!errors.empty())
  {
    for (auto &err : errors)
      gzerr << err << "\n";
    return;
  }

  // Add record plugin
  if (_config.UseLogRecord())
  {
    this->dataPtr->AddRecordPlugin(_config);
  }

  this->dataPtr->CreateEntities();

  // Set the desired update period, this will override the desired RTF given in
  // the world file which was parsed by CreateEntities.
  if (_config.UpdatePeriod())
  {
    this->SetUpdatePeriod(_config.UpdatePeriod().value());
  }

  // Establish publishers and subscribers.
  this->dataPtr->SetupTransport();
}

/////////////////////////////////////////////////
Server::~Server() = default;

/////////////////////////////////////////////////
bool Server::Run(const bool _blocking, const uint64_t _iterations,
    const bool _paused)
{
  // Set the initial pause state of each simulation runner.
  for (std::unique_ptr<SimulationRunner> &runner : this->dataPtr->simRunners)
    runner->SetPaused(_paused);

  // Check the current state, and return early if preconditions are not met.
  {
    std::lock_guard<std::mutex> lock(this->dataPtr->runMutex);
    if (!this->dataPtr->sigHandler.Initialized())
    {
      gzerr << "Signal handlers were not created. The server won't run.\n";
      return false;
    }

    // Do not allow running more than once.
    if (this->dataPtr->running)
    {
      gzwarn << "The server is already runnnng.\n";
      return false;
    }
  }

  if (_blocking)
    return this->dataPtr->Run(_iterations);

  // Make sure two threads are not created
  std::unique_lock<std::mutex> lock(this->dataPtr->runMutex);
  if (this->dataPtr->runThread.get_id() == std::thread::id())
  {
    std::condition_variable cond;
    this->dataPtr->runThread =
      std::thread(&ServerPrivate::Run, this->dataPtr.get(), _iterations, &cond);

    // Wait for the thread to start. We do this to guarantee that the
    // running variable gets updated before this function returns. With
    // a small number of iterations it is possible that the run thread
    // successfuly completes before this function returns.
    cond.wait(lock, [this]() -> bool {return this->dataPtr->running;});
    return true;
  }

  return false;
}

/////////////////////////////////////////////////
bool Server::RunOnce(const bool _paused)
{
  if (_paused)
  {
    for (auto &runner : this->dataPtr->simRunners)
      runner->SetNextStepAsBlockingPaused(true);
  }

  return this->Run(true, 1, _paused);
}

/////////////////////////////////////////////////
void Server::SetUpdatePeriod(
    const std::chrono::steady_clock::duration &_updatePeriod,
    const unsigned int _worldIndex)
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    this->dataPtr->simRunners[_worldIndex]->SetUpdatePeriod(_updatePeriod);
}

//////////////////////////////////////////////////
bool Server::Running() const
{
  return this->dataPtr->running;
}

//////////////////////////////////////////////////
std::optional<bool> Server::Running(const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->Running();
  return std::nullopt;
}

//////////////////////////////////////////////////
bool Server::SetPaused(const bool _paused,
    const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
  {
    this->dataPtr->simRunners[_worldIndex]->SetPaused(_paused);
    return true;
  }

  return false;
}

//////////////////////////////////////////////////
std::optional<bool> Server::Paused(const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->Paused();
  return std::nullopt;
}

//////////////////////////////////////////////////
std::optional<uint64_t> Server::IterationCount(
    const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->IterationCount();
  return std::nullopt;
}

//////////////////////////////////////////////////
std::optional<size_t> Server::EntityCount(const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->EntityCount();
  return std::nullopt;
}

//////////////////////////////////////////////////
std::optional<size_t> Server::SystemCount(const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->SystemCount();
  return std::nullopt;
}

//////////////////////////////////////////////////
std::optional<bool> Server::AddSystem(const SystemPluginPtr &_system,
                                      const unsigned int _worldIndex)
{
  // Check the current state, and return early if preconditions are not met.
  std::lock_guard<std::mutex> lock(this->dataPtr->runMutex);
  // Do not allow running more than once.
  if (this->dataPtr->running)
  {
    gzerr << "Cannot add system while the server is runnnng.\n";
    return false;
  }

  if (_worldIndex < this->dataPtr->simRunners.size())
  {
    this->dataPtr->simRunners[_worldIndex]->AddSystem(_system);
    return true;
  }

  return std::nullopt;
}

//////////////////////////////////////////////////
std::optional<bool> Server::AddSystem(const std::shared_ptr<System> &_system,
                                      const unsigned int _worldIndex)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->runMutex);
  if (this->dataPtr->running)
  {
    gzerr << "Cannot add system while the server is runnnng.\n";
    return false;
  }

  if (_worldIndex < this->dataPtr->simRunners.size())
  {
    this->dataPtr->simRunners[_worldIndex]->AddSystem(_system);
    return true;
  }

  return std::nullopt;
}

//////////////////////////////////////////////////
bool Server::HasEntity(const std::string &_name,
                       const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->HasEntity(_name);

  return false;
}

//////////////////////////////////////////////////
std::optional<Entity> Server::EntityByName(const std::string &_name,
    const unsigned int _worldIndex) const
{
  if (_worldIndex < this->dataPtr->simRunners.size())
    return this->dataPtr->simRunners[_worldIndex]->EntityByName(_name);

  return std::nullopt;
}

//////////////////////////////////////////////////
bool Server::RequestRemoveEntity(const std::string &_name,
    bool _recursive, const unsigned int _worldIndex)
{
  if (_worldIndex < this->dataPtr->simRunners.size())
  {
    return this->dataPtr->simRunners[_worldIndex]->RequestRemoveEntity(_name,
        _recursive);
  }

  return false;
}

//////////////////////////////////////////////////
bool Server::RequestRemoveEntity(const Entity _entity,
    bool _recursive, const unsigned int _worldIndex)
{
  if (_worldIndex < this->dataPtr->simRunners.size())
  {
    return this->dataPtr->simRunners[_worldIndex]->RequestRemoveEntity(_entity,
        _recursive);
  }

  return false;
}

//////////////////////////////////////////////////
void Server::Stop()
{
  this->dataPtr->Stop();
}
