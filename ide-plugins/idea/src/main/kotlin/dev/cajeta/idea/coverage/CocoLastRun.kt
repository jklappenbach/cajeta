package dev.cajeta.idea.coverage

import com.intellij.execution.runners.ExecutionEnvironment
import com.intellij.execution.runners.ExecutionUtil
import com.intellij.openapi.components.Service
import com.intellij.openapi.project.Project
import java.lang.ref.WeakReference

/**
 * The last coverage run, so "Re-run" can mean the same run rather than making
 * the developer find the configuration again (spec §5.3).
 *
 * Held weakly: an environment pins a run configuration and its console, and a
 * stale-coverage reminder is not a reason to keep those alive. When it has been
 * collected the offer simply is not made — better than re-running something
 * subtly different from what produced the results on screen.
 */
@Service(Service.Level.PROJECT)
class CocoLastRun {

    private var last: WeakReference<ExecutionEnvironment>? = null

    fun remember(environment: ExecutionEnvironment) {
        last = WeakReference(environment)
    }

    /** True when a re-run can actually be offered. */
    fun canRerun(): Boolean = last?.get() != null

    fun rerun(): Boolean {
        val env = last?.get() ?: return false
        ExecutionUtil.restart(env)
        return true
    }

    companion object {
        fun getInstance(project: Project): CocoLastRun =
            project.getService(CocoLastRun::class.java)
    }
}
