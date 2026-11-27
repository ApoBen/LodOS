package org.lodos.app.storage

import org.junit.Assert.*
import org.junit.Test

class StorageAccessTest {
    @Test fun api29RequiresBothLegacyPermissions() {
        assertEquals(StorageAccess.State.NEEDS_REQUEST, StorageAccess.state(29, false, true, false))
        assertEquals(StorageAccess.State.GRANTED, StorageAccess.state(29, false, true, true))
        assertEquals(StorageAccess.Request.READ_WRITE_EXTERNAL_STORAGE, StorageAccess.requestFor(29))
    }

    @Test fun api30UsesAllFilesAccess() {
        assertEquals(StorageAccess.State.NEEDS_REQUEST, StorageAccess.state(30, false, true, true))
        assertEquals(StorageAccess.State.GRANTED, StorageAccess.state(30, true, false, false))
        assertEquals(StorageAccess.Request.MANAGE_EXTERNAL_STORAGE, StorageAccess.requestFor(30))
    }

    @Test fun api34DenialAndGrantedPath() {
        assertEquals(StorageAccess.State.NEEDS_REQUEST, StorageAccess.state(34, false, false, false))
        assertNull(StorageAccess.sdcardPathOrNull(StorageAccess.State.NEEDS_REQUEST, "/storage/emulated/0"))
        assertEquals("/storage/emulated/0", StorageAccess.sdcardPathOrNull(StorageAccess.State.GRANTED, "/storage/emulated/0"))
    }

    @Test fun noneNeverProvidesPath() {
        assertNull(StorageAccess.sdcardPathOrNull(StorageAccess.State.NONE, "/storage/emulated/0"))
    }

    @Test fun api34IgnoresLegacyGrantFlags() {
        assertEquals(StorageAccess.State.NEEDS_REQUEST, StorageAccess.state(34, false, true, true))
        assertEquals(StorageAccess.State.GRANTED, StorageAccess.state(34, true, false, false))
    }
}
