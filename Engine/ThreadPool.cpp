/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ThreadPool.h"

#include <string>
#include <sstream>

#include <QtCore/QAtomicInt>
#include <QtCore/QMutex>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>

#include "Engine/Node.h"
#include "Engine/TreeRender.h"

NATRON_NAMESPACE_ENTER;


struct AbortableThreadPrivate
{
    QThread* thread;
    std::string threadName;
    mutable QMutex renderMutex;
    TreeRenderWPtr currentRender;

    std::string currentActionName;
    NodeWPtr currentActionNode;

    AbortableThreadPrivate(QThread* thread)
        : thread(thread)
        , threadName()
        , renderMutex()
        , currentRender()
        , currentActionName()
        , currentActionNode()
    {
    }
};

AbortableThread::AbortableThread(QThread* thread)
    : _imp( new AbortableThreadPrivate(thread) )
{
}

AbortableThread::~AbortableThread()
{
}

void
AbortableThread::setThreadName(const std::string& threadName)
{
    std::stringstream ss;

    ss << threadName << " (" << this << ")";
    _imp->threadName = ss.str();
    _imp->thread->setObjectName( QString::fromUtf8( _imp->threadName.c_str() ) );
}

const std::string&
AbortableThread::getThreadName() const
{
    return _imp->threadName;
}

void
AbortableThread::setCurrentActionInfos(const std::string& actionName,
                                       const NodePtr& node)
{
    assert(QThread::currentThread() == _imp->thread);

    QMutexLocker k(&_imp->renderMutex);
    _imp->currentActionName = actionName;
    _imp->currentActionNode = node;
}

void
AbortableThread::getCurrentActionInfos(std::string* actionName,
                                       NodePtr* node) const
{
    QMutexLocker k(&_imp->renderMutex);

    *actionName = _imp->currentActionName;
    *node = _imp->currentActionNode.lock();
}

void
AbortableThread::killThread()
{
    _imp->thread->terminate();
}

QThread*
AbortableThread::getThread() const
{
    return _imp->thread;
}


void
AbortableThread::setCurrentRender(const TreeRenderPtr& render)
{
    TreeRenderPtr curRender;
    {
        QMutexLocker k(&_imp->renderMutex);
        curRender = _imp->currentRender.lock();
        _imp->currentRender = render;
    }
    if (render) {
        render->registerThreadForRender(this);
    } else {
        if (curRender) {
            curRender->unregisterThreadForRender(this);
        }
    }
}


TreeRenderPtr
AbortableThread::getCurrentRender() const
{
    QMutexLocker k(&_imp->renderMutex);
    return _imp->currentRender.lock();
}



// We patched Qt to be able to derive QThreadPool to control the threads that are spawned to improve performances
// of the EffectInstance::aborted() function
#ifdef QT_CUSTOM_THREADPOOL

NATRON_NAMESPACE_ANONYMOUS_ENTER

class ThreadPoolThread
    : public QThreadPoolThread
      , public AbortableThread
{
public:

    ThreadPoolThread()
        : QThreadPoolThread()
        , AbortableThread(this)
    {
    }

    virtual bool isThreadPoolThread() const { return true; }

    virtual ~ThreadPoolThread() {}
};

NATRON_NAMESPACE_ANONYMOUS_EXIT


ThreadPool::ThreadPool()
    : QThreadPool()
{
}

ThreadPool::~ThreadPool()
{
}

QThreadPoolThread*
ThreadPool::createThreadPoolThread() const
{
    ThreadPoolThread* ret = new ThreadPoolThread();

    ret->setThreadName("Global Thread (Pooled)");

    return ret;
}

#endif // ifdef QT_CUSTOM_THREADPOOL

NATRON_NAMESPACE_EXIT;

