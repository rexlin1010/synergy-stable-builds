/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2004 Chris Schoeneman
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file COPYING that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "CEventQueue.h"
#include "CLog.h"
#include "CSimpleEventQueueBuffer.h"
#include "CStopwatch.h"
#include "IEventJob.h"
#include "CArch.h"

// interrupt handler.  this just adds a quit event to the queue.
static
void
interrupt(void*)
{
	EVENTQUEUE->addEvent(CEvent(CEvent::kQuit));
}


//
// CEventQueue
//

CEventQueue::CEventQueue() :
	m_nextType(CEvent::kLast)
{
	setInstance(this);
	m_mutex = ARCH->newMutex();
	ARCH->setInterruptHandler(&interrupt, NULL);
	m_buffer = new CSimpleEventQueueBuffer;
}

CEventQueue::~CEventQueue()
{
	delete m_buffer;
	ARCH->setInterruptHandler(NULL, NULL);
	ARCH->closeMutex(m_mutex);
	setInstance(NULL);
}

CEvent::Type
CEventQueue::registerType(const char* name)
{
	CArchMutexLock lock(m_mutex);
	m_typeMap.insert(std::make_pair(m_nextType, name));
	LOG((CLOG_DEBUG1 "registered event type %s as %d", name, m_nextType));
	return m_nextType++;
}

CEvent::Type
CEventQueue::registerTypeOnce(CEvent::Type& type, const char* name)
{
	CArchMutexLock lock(m_mutex);
	if (type == CEvent::kUnknown) {
		m_typeMap.insert(std::make_pair(m_nextType, name));
		LOG((CLOG_DEBUG1 "registered event type %s as %d", name, m_nextType));
		type = m_nextType++;
	}
	return type;
}

const char*
CEventQueue::getTypeName(CEvent::Type type)
{
	switch (type) {
	case CEvent::kUnknown:
		return "nil";

	case CEvent::kQuit:
		return "quit";

	case CEvent::kSystem:
		return "system";

	case CEvent::kTimer:
		return "timer";

	default:
		CTypeMap::const_iterator i = m_typeMap.find(type);
		if (i == m_typeMap.end()) {
			return "<unknown>";
		}
		else {
			return i->second;
		}
	}
}

void
CEventQueue::adoptBuffer(IEventQueueBuffer* buffer)
{
	CArchMutexLock lock(m_mutex);

	// discard old buffer and old events
	delete m_buffer;
	for (CEventTable::iterator i = m_events.begin(); i != m_events.end(); ++i) {
		CEvent::deleteData(i->second);
	}
	m_events.clear();
	m_oldEventIDs.clear();

	// use new buffer
	m_buffer = buffer;
	if (m_buffer == NULL) {
		m_buffer = new CSimpleEventQueueBuffer;
	}
}

bool
CEventQueue::getEvent(CEvent& event, double timeout)
{
	CStopwatch timer(true);
retry:
	// if no events are waiting then handle timers and then wait
	while (m_buffer->isEmpty()) {
		// handle timers first
		if (hasTimerExpired(event)) {
			return true;
		}

		// get time remaining in timeout
		double timeLeft = timeout - timer.getTime();
		if (timeout >= 0.0 && timeLeft <= 0.0) {
			return false;
		}

		// get time until next timer expires.  if there is a timer
		// and it'll expire before the client's timeout then use
		// that duration for our timeout instead.
		double timerTimeout = getNextTimerTimeout();
		if (timeout < 0.0 || (timerTimeout >= 0.0 && timerTimeout < timeLeft)) {
			timeLeft = timerTimeout;
		}

		// wait for an event
		m_buffer->waitForEvent(timeLeft);
	}

	// get the event
	UInt32 dataID;
	IEventQueueBuffer::Type type = m_buffer->getEvent(event, dataID);
	switch (type) {
	case IEventQueueBuffer::kNone:
		if (timeout < 0.0 || timeout <= timer.getTime()) {
			// don't want to fail if client isn't expecting that
			// so if getEvent() fails with an infinite timeout
			// then just try getting another event.
			goto retry;
		}
		return false;

	case IEventQueueBuffer::kSystem:
		return true;

	case IEventQueueBuffer::kUser:
		{
			CArchMutexLock lock(m_mutex);
			event = removeEvent(dataID);
			return true;
		}
	}
}

bool
CEventQueue::dispatchEvent(const CEvent& event)
{
	void* target   = event.getTarget();
	IEventJob* job = getHandler(event.getType(), target);
	if (job != NULL) {
		job->run(event);
		return true;
	}
	return false;
}

void
CEventQueue::addEvent(const CEvent& event)
{
	// discard bogus event types
	switch (event.getType()) {
	case CEvent::kUnknown:
	case CEvent::kSystem:
	case CEvent::kTimer:
		return;

	default:
		break;
	}

	CArchMutexLock lock(m_mutex);

	// store the event's data locally
	UInt32 eventID = saveEvent(event);

	// add it
	if (!m_buffer->addEvent(eventID)) {
		// failed to send event
		removeEvent(eventID);
		CEvent::deleteData(event);
	}
}

CEventQueueTimer*
CEventQueue::newTimer(double duration, void* target)
{
	assert(duration > 0.0);

	CEventQueueTimer* timer = m_buffer->newTimer(duration, false);
	if (target == NULL) {
		target = timer;
	}
	CArchMutexLock lock(m_mutex);
	m_timers.insert(timer);
	// initial duration is requested duration plus whatever's on
	// the clock currently because the latter will be subtracted
	// the next time we check for timers.
	m_timerQueue.push(CTimer(timer, duration,
							duration + m_time.getTime(), target, false));
	return timer;
}

CEventQueueTimer*
CEventQueue::newOneShotTimer(double duration, void* target)
{
	assert(duration > 0.0);

	CEventQueueTimer* timer = m_buffer->newTimer(duration, true);
	if (target == NULL) {
		target = timer;
	}
	CArchMutexLock lock(m_mutex);
	m_timers.insert(timer);
	// initial duration is requested duration plus whatever's on
	// the clock currently because the latter will be subtracted
	// the next time we check for timers.
	m_timerQueue.push(CTimer(timer, duration,
							duration + m_time.getTime(), target, true));
	return timer;
}

void
CEventQueue::deleteTimer(CEventQueueTimer* timer)
{
	CArchMutexLock lock(m_mutex);
	for (CTimerQueue::iterator index = m_timerQueue.begin();
							index != m_timerQueue.end(); ++index) {
		if (index->getTimer() == timer) {
			m_timerQueue.erase(index);
			break;
		}
	}
	CTimers::iterator index = m_timers.find(timer);
	if (index != m_timers.end()) {
		m_timers.erase(index);
	}
	m_buffer->deleteTimer(timer);
}

void
CEventQueue::adoptHandler(void* target, IEventJob* handler)
{
	doAdoptHandler(CEvent::kUnknown, target, handler);
}

void
CEventQueue::adoptHandler(CEvent::Type type, void* target, IEventJob* handler)
{
	assert(type != CEvent::kUnknown);
	doAdoptHandler(type, target, handler);
}

IEventJob*
CEventQueue::orphanHandler(void* target)
{
	return doOrphanHandler(CEvent::kUnknown, target);
}

IEventJob*
CEventQueue::orphanHandler(CEvent::Type type, void* target)
{
	assert(type != CEvent::kUnknown);
	return doOrphanHandler(type, target);
}

void
CEventQueue::removeHandler(void* target)
{
	delete orphanHandler(target);
}

void
CEventQueue::removeHandler(CEvent::Type type, void* target)
{
	delete orphanHandler(type, target);
}

void
CEventQueue::doAdoptHandler(CEvent::Type type, void* target, IEventJob* handler)
{
	CArchMutexLock lock(m_mutex);
	IEventJob*& job = m_handlers[CTypeTarget(type, target)];
	delete job;
	job = handler;
}

IEventJob*
CEventQueue::doOrphanHandler(CEvent::Type type, void* target)
{
	CArchMutexLock lock(m_mutex);
	CHandlerTable::iterator index = m_handlers.find(CTypeTarget(type, target));
	if (index != m_handlers.end()) {
		IEventJob* handler = index->second;
		m_handlers.erase(index);
		return handler;
	}
	else {
		return NULL;
	}
}

bool
CEventQueue::isEmpty() const
{
	return (m_buffer->isEmpty() && getNextTimerTimeout() != 0.0);
}

IEventJob*
CEventQueue::getHandler(CEvent::Type type, void* target) const
{
	CArchMutexLock lock(m_mutex);
	CHandlerTable::const_iterator index =
							m_handlers.find(CTypeTarget(type, target));
	if (index != m_handlers.end()) {
		return index->second;
	}
	index = m_handlers.find(CTypeTarget(CEvent::kUnknown, target));
	if (index != m_handlers.end()) {
		return index->second;
	}
	return NULL;
}

UInt32
CEventQueue::saveEvent(const CEvent& event)
{
	// choose id
	UInt32 id;
	if (!m_oldEventIDs.empty()) {
		// reuse an id
		id = m_oldEventIDs.back();
		m_oldEventIDs.pop_back();
	}
	else {
		// make a new id
		id = static_cast<UInt32>(m_events.size());
	}

	// save data
	m_events[id] = event;
	return id;
}

CEvent
CEventQueue::removeEvent(UInt32 eventID)
{
	// look up id
	CEventTable::iterator index = m_events.find(eventID);
	if (index == m_events.end()) {
		return CEvent();
	}

	// get data
	CEvent event = index->second;
	m_events.erase(index);

	// save old id for reuse
	m_oldEventIDs.push_back(eventID);

	return event;
}

bool
CEventQueue::hasTimerExpired(CEvent& event)
{
	// return true if there's a timer in the timer priority queue that
	// has expired.  if returning true then fill in event appropriately
	// and reset and reinsert the timer.
	if (m_timerQueue.empty()) {
		return false;
	}

	// get time elapsed since last check
	const double time = m_time.getTime();
	m_time.reset();

	// countdown elapsed time
	for (CTimerQueue::iterator index = m_timerQueue.begin();
							index != m_timerQueue.end(); ++index) {
		(*index) -= time;
	}

	// done if no timers are expired
	if (m_timerQueue.top() > 0.0) {
		return false;
	}

	// remove timer from queue
	CTimer timer = m_timerQueue.top();
	m_timerQueue.pop();

	// prepare event and reset the timer's clock
	timer.fillEvent(m_timerEvent);
	event = CEvent(CEvent::kTimer, timer.getTarget(), &m_timerEvent);
	timer.reset();

	// reinsert timer into queue if it's not a one-shot
	if (!timer.isOneShot()) {
		m_timerQueue.push(timer);
	}

	return true;
}

double
CEventQueue::getNextTimerTimeout() const
{
	// return -1 if no timers, 0 if the top timer has expired, otherwise
	// the time until the top timer in the timer priority queue will
	// expire.
	if (m_timerQueue.empty()) {
		return -1.0;
	}
	if (m_timerQueue.top() <= 0.0) {
		return 0.0;
	}
	return m_timerQueue.top();
}


//
// CEventQueue::CTypeTarget
//

CEventQueue::CTypeTarget::CTypeTarget(CEvent::Type type, void* target) :
	m_type(type),
	m_target(target)
{
	// do nothing
}

CEventQueue::CTypeTarget::~CTypeTarget()
{
	// do nothing
}

bool
CEventQueue::CTypeTarget::operator<(const CTypeTarget& tt) const
{
	return (m_type < tt.m_type ||
			(m_type == tt.m_type && m_target < tt.m_target));
}


//
// CEventQueue::CTimer
//

CEventQueue::CTimer::CTimer(CEventQueueTimer* timer, double timeout,
				double initialTime, void* target, bool oneShot) :
	m_timer(timer),
	m_timeout(timeout),
	m_target(target),
	m_oneShot(oneShot),
	m_time(initialTime)
{
	assert(m_timeout > 0.0);
}

CEventQueue::CTimer::~CTimer()
{
	// do nothing
}

void
CEventQueue::CTimer::reset()
{
	m_time = m_timeout;
}

CEventQueue::CTimer::CTimer&
CEventQueue::CTimer::operator-=(double dt)
{
	m_time -= dt;
	return *this;
}

CEventQueue::CTimer::operator double() const
{
	return m_time;
}

bool
CEventQueue::CTimer::isOneShot() const
{
	return m_oneShot;
}

CEventQueueTimer*
CEventQueue::CTimer::getTimer() const
{
	return m_timer;
}

void*
CEventQueue::CTimer::getTarget() const
{
	return m_target;
}

void
CEventQueue::CTimer::fillEvent(CTimerEvent& event) const
{
	event.m_timer = m_timer;
	event.m_count = 0;
	if (m_time <= 0.0) {
		event.m_count = static_cast<UInt32>((m_timeout - m_time) / m_timeout);
	}
}

bool
CEventQueue::CTimer::operator<(const CTimer& t) const
{
	return m_time < t.m_time;
}
