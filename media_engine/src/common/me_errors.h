#ifndef ME_ERRORS_H
#define ME_ERRORS_H

/* Wire error codes (see custom_part/docs/ipc_architecture.md 4.4). Shared by
 * every module so lower layers do not need to know about the IPC server. */
#define ME_ERR_OK 0
#define ME_ERR_MEDIA (-32000) /* media busy or media-plane failure */
#define ME_ERR_NOT_FOUND (-32001)
#define ME_ERR_PARAM (-32002)

#endif /* ME_ERRORS_H */
