/**
 * @file error.h
 * @brief Helios OS — Kernel-wide error codes.
 */

#ifndef HELIOS_ERROR_H
#define HELIOS_ERROR_H

typedef enum {
    HELIOS_OK                    =  0,
    HELIOS_ERR_GENERIC           = -1,
    HELIOS_ERR_NO_MEMORY         = -2,
    HELIOS_ERR_INVALID_ARGUMENT  = -3,
    HELIOS_ERR_NOT_FOUND         = -4,
    HELIOS_ERR_PERMISSION        = -5,
    HELIOS_ERR_BUSY              = -6,
    HELIOS_ERR_TIMEOUT           = -7,
    HELIOS_ERR_IO                = -8,
    HELIOS_ERR_INTEGRITY         = -9,
    HELIOS_ERR_CAP_BOUNDS        = -10,
    HELIOS_ERR_CAP_PERM          = -11,
    HELIOS_ERR_CAP_HMAC          = -12,
    HELIOS_ERR_CAP_REVOKED       = -13,
} helios_err_t;

#endif /* HELIOS_ERROR_H */
