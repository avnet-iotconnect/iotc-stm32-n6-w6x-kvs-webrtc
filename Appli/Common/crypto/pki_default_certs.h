/*
 * Auto-provisioning of the CA certificates this project always needs
 * (AWS IoT Root CA 1 for the AWS-hosted IOTCONNECT broker, and the
 * IOTCONNECT DRA CA used for device bootstrap/discovery). Writes them
 * to PKCS#11 storage on first boot so the "pki import cert" CLI steps
 * are no longer a required part of provisioning.
 */

#ifndef _PKI_DEFAULT_CERTS_H_
#define _PKI_DEFAULT_CERTS_H_

void vPkiProvisionDefaultCaCertificates( void );

#endif /* _PKI_DEFAULT_CERTS_H_ */
