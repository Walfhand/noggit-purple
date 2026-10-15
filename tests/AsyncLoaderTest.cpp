// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/SceneObject.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
  using namespace std::chrono_literals;

  class BlockingObject final : public AsyncObject
  {
  public:
    BlockingObject() : AsyncObject("async-loader-test") {}

    void finishLoading() override
    {
      _calls.fetch_add(1);
      {
        std::unique_lock lock(_test_mutex);
        _started.notify_all();
        _release.wait(lock, [&] { return _released; });
      }
      finished = true;
      _state_changed.notify_all();
    }

    void waitForChildrenLoaded() override {}

    bool waitForCalls(int count, std::chrono::milliseconds timeout)
    {
      std::unique_lock lock(_test_mutex);
      return _started.wait_for(lock, timeout, [&]
      {
        return _calls.load() >= count;
      });
    }

    void release()
    {
      {
        std::lock_guard lock(_test_mutex);
        _released = true;
      }
      _release.notify_all();
    }

    int calls() const { return _calls.load(); }

  private:
    std::atomic<int> _calls{0};
    std::mutex _test_mutex;
    std::condition_variable _started;
    std::condition_variable _release;
    bool _released = false;
  };

  class TileTrackingObject final : public SceneObject
  {
  public:
    TileTrackingObject() : SceneObject(eMODEL, Noggit::MAP_VIEW) {}

    void recalcExtents() override {}
    void ensureExtents() override {}
    bool finishedLoading() override { return true; }
    AsyncObject* instance_model() const override { return nullptr; }
    std::array<glm::vec3, 8> getBoundingBox() override { return {}; }
    void updateDetails(Noggit::Ui::detail_infos*) override {}
  };

  class ExtentTrackingObject final : public SceneObject
  {
  public:
    ExtentTrackingObject() : SceneObject(eMODEL, Noggit::MAP_VIEW) {}

    void recalcExtents() override
    {
      std::lock_guard lock(_extents_mutex);
      if (_active.fetch_add(1) != 0)
        _overlapped = true;

      auto const generation = static_cast<float>(++_generation);
      extents[0] = glm::vec3(generation);
      std::this_thread::yield();
      extents[1] = glm::vec3(generation);
      _active.fetch_sub(1);
    }

    void ensureExtents() override { recalcExtents(); }
    bool finishedLoading() override { return true; }
    AsyncObject* instance_model() const override { return nullptr; }
    std::array<glm::vec3, 8> getBoundingBox() override { return {}; }
    void updateDetails(Noggit::Ui::detail_infos*) override {}

    bool overlapped() const { return _overlapped.load(); }

  private:
    std::atomic<int> _active{0};
    std::atomic<bool> _overlapped{false};
    int _generation = 0;
  };

  void require(bool condition, char const* message)
  {
    if (!condition) throw std::runtime_error(message);
  }
}

int main()
{
  {
    BlockingObject object;
    AsyncLoader loader(1);
    loader.queue_for_load(&object);
    auto const started = object.waitForCalls(1, 2s);
    if (started) loader.queue_for_load(&object);

    std::atomic<bool> deletable{false};
    std::mutex waiter_mutex;
    std::condition_variable waiter_state;
    bool waiter_started = false;
    auto waiter = std::thread{};
    if (started)
    {
      waiter = std::thread([&]
      {
        {
          std::lock_guard lock(waiter_mutex);
          waiter_started = true;
        }
        waiter_state.notify_all();
        loader.ensure_deletable(&object);
        deletable = true;
        waiter_state.notify_all();
      });
    }

    auto returned_while_loading = false;
    if (started)
    {
      std::unique_lock lock(waiter_mutex);
      waiter_state.wait(lock, [&] { return waiter_started; });
      returned_while_loading = waiter_state.wait_for(
        lock, 1s, [&] { return deletable.load(); });
    }
    object.release();
    if (waiter.joinable()) waiter.join();

    require(started, "the queued object never started");
    require(!returned_while_loading,
      "ensure_deletable returned while the object was still loading");
    require(object.calls() == 1,
      "ensure_deletable left a duplicate queued load behind");
  }

  {
    BlockingObject object;
    AsyncLoader loader(2);
    loader.queue_for_load(&object);
    auto const started = object.waitForCalls(1, 2s);
    if (started) loader.queue_for_load(&object);
    object.release();
    loader.ensure_deletable(&object);
    require(started, "the queued object never started");
    require(object.calls() == 1, "the same object was loaded concurrently");
  }

  {
    TileTrackingObject object;
    auto identities = std::array<std::uintptr_t, 16>{};
    auto tiles = std::array<MapTile*, 16>{};
    for (std::size_t index = 0; index < tiles.size(); ++index)
      tiles[index] = reinterpret_cast<MapTile*>(&identities[index]);

    auto workers = std::vector<std::thread>{};
    for (auto worker = 0; worker < 4; ++worker)
      workers.emplace_back([&]
      {
        for (auto iteration = 0; iteration < 1000; ++iteration)
          for (auto* tile : tiles) object.refTile(tile);
      });
    for (auto& worker : workers) worker.join();
    require(object.getTiles().size() == tiles.size(),
      "concurrent tile references were lost or duplicated");

    object.derefTile(tiles.front());
    require(object.getTiles().size() == tiles.size() - 1,
      "an object spanning ADTs lost every tile reference at the first unload");
    object.refTile(tiles.front());

    auto copy = object;
    require(copy.getTiles().size() == tiles.size(),
      "copying a scene object lost its tile references");

    workers.clear();
    for (auto worker = 0; worker < 4; ++worker)
      workers.emplace_back([&]
      {
        for (auto* tile : tiles) object.derefTile(tile);
      });
    for (auto& worker : workers) worker.join();
    require(object.getTiles().empty(),
      "concurrent tile dereferences left stale references");
  }

  {
    ExtentTrackingObject object;
    std::atomic<bool> torn_snapshot{false};
    auto workers = std::vector<std::thread>{};
    for (auto worker = 0; worker < 4; ++worker)
      workers.emplace_back([&, worker]
      {
        for (auto iteration = 0; iteration < 1000; ++iteration)
        {
          if (worker % 2 == 0)
          {
            object.recalcExtents();
          }
          else
          {
            auto const snapshot = object.getExtents();
            if (snapshot[0] != snapshot[1])
              torn_snapshot = true;
          }
        }
      });
    for (auto& worker : workers) worker.join();

    require(!object.overlapped(),
      "extent recalculations ran concurrently");
    require(!torn_snapshot.load(),
      "getExtents returned a torn extent snapshot");
  }
}
