package org.lodos.app.storage

/** Storage permission decision kept free of Android framework dependencies for JVM tests. */
object StorageAccess {
    enum class State { NONE, GRANTED, NEEDS_REQUEST }
    enum class Request { MANAGE_EXTERNAL_STORAGE, READ_WRITE_EXTERNAL_STORAGE }

    fun state(sdk: Int, manageGranted: Boolean, readGranted: Boolean, writeGranted: Boolean): State =
        if (sdk >= 30) {
            if (manageGranted) State.GRANTED else State.NEEDS_REQUEST
        } else if (readGranted && writeGranted) State.GRANTED else State.NEEDS_REQUEST

    fun requestFor(sdk: Int): Request =
        if (sdk >= 30) Request.MANAGE_EXTERNAL_STORAGE else Request.READ_WRITE_EXTERNAL_STORAGE

    fun sdcardPathOrNull(state: State, externalDir: String): String? =
        externalDir.takeIf { state == State.GRANTED }
}
