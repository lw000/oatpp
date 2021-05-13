/***************************************************************************
 *
 * Project         _____    __   ____   _      _
 *                (  _  )  /__\ (_  _)_| |_  _| |_
 *                 )(_)(  /(__)\  )( (_   _)(_   _)
 *                (_____)(__)(__)(__)  |_|    |_|
 *
 *
 * Copyright 2018-present, Leonid Stryzhevskyi <lganzzzo@gmail.com>,
 * Matthias Haselmaier <mhaselmaier@gmail.com>
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
 ***************************************************************************/

#include "CoroutineWaitList.hpp"

#include "./Processor.hpp"
#include <set>

namespace oatpp { namespace async {


CoroutineWaitList::CoroutineWaitList(CoroutineWaitList&& other) {
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock{other.m_lock};
    m_list = std::move(other.m_list);
  }
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock{other.m_timeoutsLock};
    m_coroutinesWithTimeout = std::move(other.m_coroutinesWithTimeout);
    if (!m_coroutinesWithTimeout.empty()) {
      startTimeoutCheckerThread();
    }
  }  
}

CoroutineWaitList::~CoroutineWaitList() {
  notifyAll();
  m_stop = true;
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

void CoroutineWaitList::startTimeoutCheckerThread() {
  m_thread = std::thread{[this]() { checkCoroutinesForTimeouts(); }};
}

void CoroutineWaitList::checkCoroutinesForTimeouts() {
  while (!m_stop) {
    std::set<CoroutineHandle*> timedoutCoroutines;
    {
      std::lock_guard<oatpp::concurrency::SpinLock> lock{m_timeoutsLock};
      const auto currentTimeSinceEpochMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      const auto newEndIt = std::remove_if(std::begin(m_coroutinesWithTimeout), std::end(m_coroutinesWithTimeout), [&](const auto& entry) {
        if (currentTimeSinceEpochMS > entry.second) {
          timedoutCoroutines.insert(entry.first);
          return true;
        }
        return false;
      });
      m_coroutinesWithTimeout.erase(newEndIt, std::end(m_coroutinesWithTimeout));
    }
    if (!timedoutCoroutines.empty()) {
      std::lock_guard<oatpp::concurrency::SpinLock> lock{m_lock};
      CoroutineHandle* prev = nullptr;
      CoroutineHandle* curr = m_list.first;
      while (curr) {
        if (timedoutCoroutines.count(curr)) {
          m_list.cutEntry(curr, prev);
          curr->_PP->pushOneTask(curr);
        }
        prev = curr;
        curr = curr->_ref;
      }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void CoroutineWaitList::setListener(Listener* listener) {
  m_listener = listener;
}

void CoroutineWaitList::pushFront(CoroutineHandle* coroutine) {
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock(m_lock);
    m_list.pushFront(coroutine);
  }
  if(m_listener != nullptr) {
    m_listener->onNewItem(*this);
  }
}

void CoroutineWaitList::pushFront(CoroutineHandle* coroutine, v_int64 timeoutTimeSinceEpochMS) {
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock{m_timeoutsLock};
    m_coroutinesWithTimeout.emplace_back(coroutine, timeoutTimeSinceEpochMS);
    if (m_coroutinesWithTimeout.size() == 1 && !m_thread.joinable()) {
      startTimeoutCheckerThread();
    }
  }
  pushFront(coroutine);
}

void CoroutineWaitList::pushBack(CoroutineHandle* coroutine) {
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock(m_lock);
    m_list.pushBack(coroutine);
  }
  if(m_listener != nullptr) {
    m_listener->onNewItem(*this);
  }
}

void CoroutineWaitList::pushBack(CoroutineHandle* coroutine, v_int64 timeoutTimeSinceEpochMS) {
  {
    std::lock_guard<oatpp::concurrency::SpinLock> lock{m_timeoutsLock};
    m_coroutinesWithTimeout.emplace_back(coroutine, timeoutTimeSinceEpochMS);
    if (m_coroutinesWithTimeout.size() == 1 && !m_thread.joinable()) {
      startTimeoutCheckerThread();
    }
  }
  pushBack(coroutine);
}

void CoroutineWaitList::notifyFirst() {
  std::lock_guard<oatpp::concurrency::SpinLock> lock(m_lock);
  if(m_list.first) {
    auto coroutine = m_list.popFront();
    coroutine->_PP->pushOneTask(coroutine);
  }
}

void CoroutineWaitList::notifyAll() {
  std::lock_guard<oatpp::concurrency::SpinLock> lock(m_lock);
    while (!m_list.empty()) {
        auto curr = m_list.popFront();
        curr->_PP->pushOneTask(curr);
    }
}


}}