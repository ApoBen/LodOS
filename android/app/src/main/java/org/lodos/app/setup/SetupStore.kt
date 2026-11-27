package org.lodos.app.setup

/**
 * Key-value persistence abstraction for setup wizard state.
 * Pure Kotlin so [SetupStateMachine] can be unit-tested on the JVM without Android dependencies.
 */
interface SetupStore {
    fun getString(key: String, default: String? = null): String?
    fun putString(key: String, value: String?)
    fun getInt(key: String, default: Int = 0): Int
    fun putInt(key: String, value: Int)
    fun getBoolean(key: String, default: Boolean = false): Boolean
    fun putBoolean(key: String, value: Boolean)
    fun clear()
}
