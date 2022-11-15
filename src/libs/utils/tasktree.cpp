// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

#include "tasktree.h"

#include "guard.h"
#include "qtcassert.h"

namespace Utils {
namespace Tasking {

ExecuteInSequence sequential;
ExecuteInParallel parallel;
WorkflowPolicy stopOnError(TaskItem::WorkflowPolicy::StopOnError);
WorkflowPolicy continueOnError(TaskItem::WorkflowPolicy::ContinueOnError);
WorkflowPolicy stopOnDone(TaskItem::WorkflowPolicy::StopOnDone);
WorkflowPolicy continueOnDone(TaskItem::WorkflowPolicy::ContinueOnDone);
WorkflowPolicy optional(TaskItem::WorkflowPolicy::Optional);

void TaskItem::addChildren(const QList<TaskItem> &children)
{
    QTC_ASSERT(m_type == Type::Group, qWarning("Only Task may have children, skipping..."); return);
    for (const TaskItem &child : children) {
        switch (child.m_type) {
        case Type::Group:
            m_children.append(child);
            break;
        case Type::Mode:
            QTC_ASSERT(m_type == Type::Group,
                       qWarning("Mode may only be a child of Group, skipping..."); return);
            m_executeMode = child.m_executeMode; // TODO: Assert on redefinition?
            break;
        case Type::Policy:
            QTC_ASSERT(m_type == Type::Group,
                       qWarning("Workflow Policy may only be a child of Group, skipping..."); return);
            m_workflowPolicy = child.m_workflowPolicy; // TODO: Assert on redefinition?
            break;
        case Type::TaskHandler:
            QTC_ASSERT(child.m_taskHandler.m_createHandler,
                       qWarning("Task Create Handler can't be null, skipping..."); return);
            QTC_ASSERT(child.m_taskHandler.m_setupHandler,
                       qWarning("Task Setup Handler can't be null, skipping..."); return);
            m_children.append(child);
            break;
        case Type::GroupHandler:
            QTC_ASSERT(m_type == Type::Group, qWarning("Group Handler may only be a "
                       "child of Group, skipping..."); break);
            QTC_ASSERT(!child.m_groupHandler.m_simpleSetupHandler
                       || !m_groupHandler.m_simpleSetupHandler,
                       qWarning("Group Setup Handler redefinition, overriding..."));
            QTC_ASSERT(!child.m_groupHandler.m_simpleDoneHandler
                       || !m_groupHandler.m_simpleDoneHandler,
                       qWarning("Group Done Handler redefinition, overriding..."));
            QTC_ASSERT(!child.m_groupHandler.m_simpleErrorHandler
                       || !m_groupHandler.m_simpleErrorHandler,
                       qWarning("Group Error Handler redefinition, overriding..."));
            if (child.m_groupHandler.m_simpleSetupHandler)
                m_groupHandler.m_simpleSetupHandler = child.m_groupHandler.m_simpleSetupHandler;
            if (child.m_groupHandler.m_simpleDoneHandler)
                m_groupHandler.m_simpleDoneHandler = child.m_groupHandler.m_simpleDoneHandler;
            if (child.m_groupHandler.m_simpleErrorHandler)
                m_groupHandler.m_simpleErrorHandler = child.m_groupHandler.m_simpleErrorHandler;
            break;
        }
    }
}

} // namespace Tasking

using namespace Tasking;

class TaskTreePrivate;
class TaskNode;

class TaskContainer
{
public:
    TaskContainer(TaskTreePrivate *taskTreePrivate, TaskContainer *parentContainer,
                  const TaskItem &task);
    ~TaskContainer();
    void start();
    void stop();
    bool isRunning() const;
    void childDone(bool success);
    void invokeSubTreeHandler(bool success);
    void resetSuccessBit();
    void updateSuccessBit(bool success);

    TaskTreePrivate *m_taskTreePrivate = nullptr;
    TaskContainer *m_parentContainer = nullptr;
    const TaskItem::ExecuteMode m_executeMode = TaskItem::ExecuteMode::Parallel;
    TaskItem::WorkflowPolicy m_workflowPolicy = TaskItem::WorkflowPolicy::StopOnError;
    const TaskItem::GroupHandler m_groupHandler;
    QList<TaskNode *> m_children;
    int m_currentIndex = -1;
    bool m_successBit = true;
};

class TaskNode : public QObject
{
public:
    TaskNode(TaskTreePrivate *taskTreePrivate, TaskContainer *parentContainer,
             const TaskItem &task)
        : m_taskHandler(task.taskHandler())
        , m_container(taskTreePrivate, parentContainer, task)
    {
    }

    bool start();
    void stop();
    bool isRunning();

private:
    const TaskItem::TaskHandler m_taskHandler;
    TaskContainer m_container;
    std::unique_ptr<TaskInterface> m_task;
};

class TaskTreePrivate
{
public:
    TaskTreePrivate(TaskTree *taskTree, const Group &root)
        : q(taskTree)
        , m_root(this, nullptr, root) {}

    void emitDone() {
        GuardLocker locker(m_guard);
        emit q->done();
    }
    void emitError() {
        GuardLocker locker(m_guard);
        emit q->errorOccurred();
    }

    TaskTree *q = nullptr;
    TaskNode m_root;
    Guard m_guard;
};

TaskContainer::TaskContainer(TaskTreePrivate *taskTreePrivate, TaskContainer *parentContainer,
                             const TaskItem &task)
    : m_taskTreePrivate(taskTreePrivate)
    , m_parentContainer(parentContainer)
    , m_executeMode(task.executeMode())
    , m_workflowPolicy(task.workflowPolicy())
    , m_groupHandler(task.groupHandler())
{
    const QList<TaskItem> &children = task.children();
    for (const TaskItem &child : children)
        m_children.append(new TaskNode(m_taskTreePrivate, this, child));
}

TaskContainer::~TaskContainer()
{
    qDeleteAll(m_children);
}

void TaskContainer::start()
{
    if (m_groupHandler.m_simpleSetupHandler) {
        GuardLocker locker(m_taskTreePrivate->m_guard);
        m_groupHandler.m_simpleSetupHandler();
    }

    if (m_children.isEmpty()) {
        invokeSubTreeHandler(true);
        return;
    }

    m_currentIndex = 0;
    resetSuccessBit();

    if (m_executeMode == TaskItem::ExecuteMode::Sequential) {
        m_children.at(m_currentIndex)->start();
        return;
    }

    // Parallel case
    for (TaskNode *child : std::as_const(m_children)) {
        if (!child->start())
            return;
    }
}

void TaskContainer::stop()
{
    m_currentIndex = -1;
    for (TaskNode *child : std::as_const(m_children))
        child->stop();
}

bool TaskContainer::isRunning() const
{
    return m_currentIndex >= 0;
}

void TaskContainer::childDone(bool success)
{
    if ((m_workflowPolicy == TaskItem::WorkflowPolicy::StopOnDone && success)
            || (m_workflowPolicy == TaskItem::WorkflowPolicy::StopOnError && !success)) {
        stop();
        invokeSubTreeHandler(success);
        return;
    }

    ++m_currentIndex;
    updateSuccessBit(success);

    if (m_currentIndex == m_children.size()) {
        invokeSubTreeHandler(m_successBit);
        return;
    }

    if (m_executeMode == TaskItem::ExecuteMode::Sequential)
        m_children.at(m_currentIndex)->start();
}

void TaskContainer::invokeSubTreeHandler(bool success)
{
    m_currentIndex = -1;
    m_successBit = success;
    if (success && m_groupHandler.m_simpleDoneHandler) {
        GuardLocker locker(m_taskTreePrivate->m_guard);
        m_groupHandler.m_simpleDoneHandler();
    } else if (!success && m_groupHandler.m_simpleErrorHandler) {
        GuardLocker locker(m_taskTreePrivate->m_guard);
        m_groupHandler.m_simpleErrorHandler();
    }
    if (m_parentContainer) {
        m_parentContainer->childDone(success);
        return;
    }
    if (success)
        m_taskTreePrivate->emitDone();
    else
        m_taskTreePrivate->emitError();
}

void TaskContainer::resetSuccessBit()
{
    if (m_children.isEmpty())
        m_successBit = true;

    if (m_workflowPolicy == TaskItem::WorkflowPolicy::StopOnDone
            || m_workflowPolicy == TaskItem::WorkflowPolicy::ContinueOnDone) {
        m_successBit = false;
    } else {
        m_successBit = true;
    }
}

void TaskContainer::updateSuccessBit(bool success)
{
    if (m_workflowPolicy == TaskItem::WorkflowPolicy::Optional)
        return;
    if (m_workflowPolicy == TaskItem::WorkflowPolicy::StopOnDone
            || m_workflowPolicy == TaskItem::WorkflowPolicy::ContinueOnDone) {
        m_successBit = m_successBit || success;
    } else {
        m_successBit = m_successBit && success;
    }
}


bool TaskNode::start()
{
    if (!m_taskHandler.m_createHandler || !m_taskHandler.m_setupHandler) {
        m_container.start();
        return true;
    }
    m_task.reset(m_taskHandler.m_createHandler());
    {
        GuardLocker locker(m_container.m_taskTreePrivate->m_guard);
        m_taskHandler.m_setupHandler(*m_task.get());
    }
    connect(m_task.get(), &TaskInterface::done, this, [this](bool success) {
        if (success && m_taskHandler.m_doneHandler) {
            GuardLocker locker(m_container.m_taskTreePrivate->m_guard);
            m_taskHandler.m_doneHandler(*m_task.get());
        } else if (!success && m_taskHandler.m_errorHandler) {
            GuardLocker locker(m_container.m_taskTreePrivate->m_guard);
            m_taskHandler.m_errorHandler(*m_task.get());
        }

        m_task.release()->deleteLater();

        QTC_CHECK(m_container.m_parentContainer);
        m_container.m_parentContainer->childDone(success);
    });

    m_task->start();
    return m_task.get(); // In case of failed to start, done handler already released process
}

void TaskNode::stop()
{
    m_task.reset();
    m_container.stop();
}

bool TaskNode::isRunning()
{
    return m_task || m_container.isRunning();
}

/*!
    \class Utils::TaskTree

    \brief The TaskTree class is responsible for running async task tree structure defined in a
           declarative way.

    The Tasking namespace (similar to Layouting) is designer for building declarative task
    tree structure. The examples of tasks that can be used inside TaskTree are e.g. QtcProcess,
    FileTransfer, AsyncTask<>. It's extensible, so any possible asynchronous task may be
    integrated and used inside TaskTree. TaskTree enables to form sophisticated mixtures of
    parallel or sequential flow of tasks in tree form.

    The TaskTree consist of Group root element. The Group can have nested Group elements.
    The Group may also contain any number of tasks, e.g. Process, FileTransfer,
    AsyncTask<ReturnType>.

    Each Group can contain various other elements describing the processing flow.

    The execute mode elements of a Group specify how direct children of a Group will be executed.
    The "sequential" element of a Group means all tasks in a group will be executed in chain,
    so after the previous task finished, the next will be started. This is the default Group
    behavior. The "parallel" element of a Group means that all tasks in a Group will be started
    simultaneously. When having nested Groups hierarchy, we may mix execute modes freely
    and each Group will be executed according to its own execute mode.
    The "sequential" mode may be very useful in cases when result data from one task is need to
    be passed as an input data to the other task - sequential mode guarantees that the next
    task will be started only after the previous task has already finished.

    There are many possible "workflow" behaviors for the Group. E.g. "stopOnError",
    the default Group workflow behavior, means that whenever any direct child of a Group
    finished with error, we immediately stop processing other tasks in this group
    (in parallel case) by canceling them and immediately finish the Group with error.

    The user of TaskTree specifies how to setup his tasks (by providing TaskSetupHandlers)
    and how to collect output data from the finished tasks (by providing TaskEndHandlers).
    The user don't need to create tasks manually - TaskTree will create them when it's needed
    and destroy when they are not used anymore.

    Whenever a Group elemenent is being started, the Group's OnGroupSetup handler is being called.
    Just after the handler finishes, all Group's children are executed (either in parallel or
    in sequence). When all Group's children finished, one of Group's OnGroupDone or OnGroupError
    is being executed, depending on results of children execution and Group's workflow policy.
*/

TaskTree::TaskTree(const Group &root)
    : d(new TaskTreePrivate(this, root))
{
}

TaskTree::~TaskTree()
{
    QTC_ASSERT(!d->m_guard.isLocked(), qWarning("Deleting TaskTree instance directly from "
               "one of its handlers will lead to crash!"));
    delete d;
}

void TaskTree::start()
{
    QTC_ASSERT(!isRunning(), qWarning("The TaskTree is already running, ignoring..."); return);
    QTC_ASSERT(!d->m_guard.isLocked(), qWarning("The start() is called from one of the"
                                                "TaskTree handlers, ingoring..."); return);
    d->m_root.start();
}

void TaskTree::stop()
{
    QTC_ASSERT(!d->m_guard.isLocked(), qWarning("The stop() is called from one of the"
                                                "TaskTree handlers, ingoring..."); return);
    d->m_root.stop();
}

bool TaskTree::isRunning() const
{
    return d->m_root.isRunning();
}

} // namespace Utils