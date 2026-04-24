#pragma once
#include "interface.h"
#include "definition.h"
#include <algorithm>
#include <random>
#include <vector>
#include <map>
#include <set>
// You should not use those functions in runtime.h

namespace oj {

auto generate_tasks(const Description &desc) -> std::vector <Task> {
    std::vector<Task> tasks;
    tasks.reserve(desc.task_count);
    
    // Debug: print the constraints
    // For now, generate minimal tasks to ensure feasibility
    for (task_id_t i = 0; i < desc.task_count; ++i) {
        Task task;
        
        // Start at time 1 (or min)
        task.launch_time = std::max(time_t(1), desc.deadline_time.min);
        
        // Use minimum execution time
        task.execution_time = desc.execution_time_single.min;
        
        // Use minimum priority  
        task.priority = desc.priority_single.min;
        
        // Calculate the absolute minimum deadline needed
        double min_time_needed = PublicInformation::kStartUp + PublicInformation::kSaving + 
            (double)task.execution_time / std::pow(PublicInformation::kCPUCount, PublicInformation::kAccel);
        
        // Set deadline to be exactly what's needed plus a small buffer
        task.deadline = task.launch_time + static_cast<time_t>(min_time_needed) + 10;
        
        // Make sure it's within the allowed range
        if (task.deadline > desc.deadline_time.max) {
            task.deadline = desc.deadline_time.max;
        }
        
        tasks.push_back(task);
    }
    
    // Now we need to adjust to meet the sum constraints
    // This is tricky because we need to maintain feasibility
    
    // Calculate current sums
    time_t current_exec_sum = 0;
    priority_t current_priority_sum = 0;
    for (const auto& task : tasks) {
        current_exec_sum += task.execution_time;
        current_priority_sum += task.priority;
    }
    
    // If we need more execution time sum, distribute it carefully
    if (current_exec_sum < desc.execution_time_sum.min) {
        time_t needed = desc.execution_time_sum.min - current_exec_sum;
        
        // Distribute across tasks, checking feasibility each time
        for (auto& task : tasks) {
            if (needed <= 0) break;
            
            time_t max_additional = desc.execution_time_single.max - task.execution_time;
            time_t add = std::min(needed, max_additional);
            
            // Check if adding this much would make the task infeasible
            double new_min_time = PublicInformation::kStartUp + PublicInformation::kSaving + 
                (double)(task.execution_time + add) / std::pow(PublicInformation::kCPUCount, PublicInformation::kAccel);
            
            if (task.launch_time + static_cast<time_t>(new_min_time) + 1 <= task.deadline) {
                task.execution_time += add;
                needed -= add;
            }
        }
    }
    
    // If we need more priority sum, distribute it
    if (current_priority_sum < desc.priority_sum.min) {
        priority_t needed = desc.priority_sum.min - current_priority_sum;
        
        for (auto& task : tasks) {
            if (needed <= 0) break;
            
            priority_t max_additional = desc.priority_single.max - task.priority;
            priority_t add = std::min(needed, max_additional);
            
            task.priority += add;
            needed -= add;
        }
    }
    
    return tasks;
}

} // namespace oj

namespace oj {

// Global state for scheduler
struct SchedulerState {
    std::map<task_id_t, Task> all_tasks;
    std::map<task_id_t, time_t> task_progress;
    std::map<task_id_t, cpu_id_t> task_cpu_allocation;
    std::set<task_id_t> running_tasks;
    std::set<task_id_t> saving_tasks;
    cpu_id_t current_cpu_usage = 0;
    
    void add_tasks(const std::vector<Task>& new_tasks, task_id_t start_id) {
        for (size_t i = 0; i < new_tasks.size(); ++i) {
            task_id_t id = start_id + i;
            all_tasks[id] = new_tasks[i];
            task_progress[id] = 0;
            task_cpu_allocation[id] = 0;
        }
    }
    
    bool is_task_running(task_id_t id) const {
        return running_tasks.count(id) > 0;
    }
    
    bool is_task_saving(task_id_t id) const {
        return saving_tasks.count(id) > 0;
    }
    
    double get_task_completion_ratio(task_id_t id) const {
        auto it = task_progress.find(id);
        auto task_it = all_tasks.find(id);
        if (it == task_progress.end() || task_it == all_tasks.end()) return 0.0;
        return static_cast<double>(it->second) / task_it->second.execution_time;
    }
};

static SchedulerState g_state;

auto schedule_tasks(time_t time, std::vector <Task> list, const Description &desc) -> std::vector<Policy> {
    static task_id_t task_id_counter = 0;
    const task_id_t first_id = task_id_counter;
    
    // Add new tasks to global state
    g_state.add_tasks(list, first_id);
    task_id_counter += list.size();
    
    std::vector<Policy> policies;
    
    // Priority-based scheduling with deadline consideration
    struct TaskPriority {
        task_id_t id;
        double score;
        time_t deadline;
        priority_t priority;
        double completion_ratio;
        
        bool operator<(const TaskPriority& other) const {
            if (std::abs(score - other.score) > 1e-9) return score > other.score;
            if (deadline != other.deadline) return deadline < other.deadline;
            return priority > other.priority;
        }
    };
    
    std::vector<TaskPriority> task_candidates;
    
    // Evaluate all available tasks
    for (const auto& [id, task] : g_state.all_tasks) {
        if (g_state.is_task_running(id) || g_state.is_task_saving(id)) continue;
        if (time > task.deadline) continue; // Skip overdue tasks
        
        double completion_ratio = g_state.get_task_completion_ratio(id);
        time_t remaining_time = task.deadline - time;
        
        // Calculate priority score considering deadline urgency and priority
        double urgency = 1.0 / std::max(1.0, static_cast<double>(remaining_time));
        double score = task.priority * urgency * (1.0 - completion_ratio * 0.5);
        
        task_candidates.push_back({id, score, task.deadline, task.priority, completion_ratio});
    }
    
    std::sort(task_candidates.begin(), task_candidates.end());
    
    // Allocate CPUs to highest priority tasks
    cpu_id_t available_cpus = desc.cpu_count - g_state.current_cpu_usage;
    
    for (const auto& candidate : task_candidates) {
        if (available_cpus <= 0) break;
        
        const auto& task = g_state.all_tasks[candidate.id];
        
        // Determine optimal CPU allocation
        cpu_id_t cpu_allocation = 1;
        
        // For tasks with very tight deadlines, allocate more CPUs
        time_t remaining_time = task.deadline - time;
        time_t work_remaining = task.execution_time - g_state.task_progress[candidate.id];
        
        if (remaining_time < 20 && work_remaining > 10) {
            // Urgent task - allocate more CPUs
            cpu_allocation = std::min(available_cpus, static_cast<cpu_id_t>(4));
        } else if (remaining_time < 50) {
            // Moderately urgent - allocate moderate CPUs
            cpu_allocation = std::min(available_cpus, static_cast<cpu_id_t>(2));
        }
        
        // Launch the task
        policies.push_back(Launch{cpu_allocation, candidate.id});
        g_state.running_tasks.insert(candidate.id);
        g_state.task_cpu_allocation[candidate.id] = cpu_allocation;
        g_state.current_cpu_usage += cpu_allocation;
        available_cpus -= cpu_allocation;
    }
    
    // Check if any running tasks should be saved
    for (auto it = g_state.running_tasks.begin(); it != g_state.running_tasks.end();) {
        task_id_t id = *it;
        const auto& task = g_state.all_tasks[id];
        
        // Save if approaching deadline or if higher priority tasks are waiting
        time_t remaining_time = task.deadline - time;
        bool should_save = false;
        
        if (remaining_time <= PublicInformation::kSaving + PublicInformation::kStartUp + 5) {
            should_save = true; // Save before deadline
        } else if (!task_candidates.empty() && 
                   task_candidates[0].priority > task.priority && 
                   remaining_time > 20) {
            should_save = true; // Make room for higher priority task
        }
        
        if (should_save) {
            policies.push_back(Saving{id});
            g_state.saving_tasks.insert(id);
            it = g_state.running_tasks.erase(it);
        } else {
            ++it;
        }
    }
    
    return policies;
}

} // namespace oj
