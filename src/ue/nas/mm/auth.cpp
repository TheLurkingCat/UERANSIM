//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "mm.hpp"
#include <openssl/ssl.h>
#include <openssl/store.h>
#include <openssl/ui.h>

#include <lib/nas/utils.hpp>
#include <ue/nas/keys.hpp>

namespace
{

int uiReader(UI *ui, UI_STRING *uis)
{
    std::string *password;
    switch (UI_get_string_type(uis))
    {
    case UIT_PROMPT:
    case UIT_VERIFY:
        password = reinterpret_cast<std::string *>(UI_get0_user_data(ui));
        if (password && (UI_get_input_flags(uis) & UI_INPUT_FLAG_DEFAULT_PWD))
        {
            UI_set_result(ui, uis, password->c_str());
            return 1;
        }
    default:
        break;
    }
    return (UI_method_get_reader(UI_OpenSSL()))(ui, uis);
}

int uiWriter(UI *ui, UI_STRING *uis)
{
    switch (UI_get_string_type(uis))
    {
    case UIT_PROMPT:
    case UIT_VERIFY:
        if (UI_get0_user_data(ui) && (UI_get_input_flags(uis) & UI_INPUT_FLAG_DEFAULT_PWD))
        {
            return 1;
        }
    default:
        break;
    }
    return (UI_method_get_writer(UI_OpenSSL()))(ui, uis);
}

UI_METHOD *makeMethod()
{
    UI_METHOD *method = UI_create_method("TPM User Interface");
    UI_method_set_opener(method, UI_method_get_opener(UI_OpenSSL()));
    UI_method_set_closer(method, UI_method_get_closer(UI_OpenSSL()));
    UI_method_set_reader(method, uiReader);
    UI_method_set_writer(method, uiWriter);
    return method;
}
} // namespace

namespace nr::ue
{

void NasMm::receiveAuthenticationRequest(const nas::AuthenticationRequest &msg)
{
    m_logger->debug("Authentication Request received");

    if (!m_usim->isValid())
    {
        m_logger->warn("Authentication request is ignored. USIM is invalid");
        return;
    }

    m_timers->t3520.start();

    if (msg.eapMessage.has_value())
        receiveAuthenticationRequestEap(msg);
    else
        receiveAuthenticationRequest5gAka(msg);
}

void NasMm::receiveAuthenticationRequestEap(const nas::AuthenticationRequest &msg)
{
    Plmn currentPlmn = m_base->shCtx.getCurrentPlmn();
    if (!currentPlmn.hasValue())
        return;

    auto sendEapFailure = [this](std::unique_ptr<eap::Eap> &&eap) {
        // Clear RAND and RES* stored in volatile memory
        m_usim->m_rand = {};
        m_usim->m_resStar = {};

        // Stop T3516 if running
        m_timers->t3516.stop();

        nas::AuthenticationResponse resp;
        resp.eapMessage = nas::IEEapMessage{};
        resp.eapMessage->eap = std::move(eap);
        sendNasMessage(resp);
    };

    auto sendAuthFailure = [this](nas::EMmCause cause) {
        m_logger->err("Sending Authentication Failure with cause [%s]", nas::utils::EnumToString(cause));

        // Clear RAND and RES* stored in volatile memory
        m_usim->m_rand = {};
        m_usim->m_resStar = {};

        // Stop T3516 if running
        m_timers->t3516.stop();

        // Send Authentication Failure
        nas::AuthenticationFailure resp{};
        resp.mmCause.value = cause;
        sendNasMessage(resp);
    };

    // ========================== Check the received message syntactically ==========================

    if (!msg.eapMessage.has_value())
    {
        sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
        return;
    }

    if (msg.eapMessage->eap->eapType == eap::EEapType::EAP_AKA_PRIME)
    {
        auto &receivedEap = (const eap::EapAkaPrime &)*msg.eapMessage->eap;
        if (receivedEap.subType != eap::ESubType::AKA_CHALLENGE)
        {
            sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
            return;
        }

        // ================================ Check the received parameters syntactically ================================

        auto receivedRand = receivedEap.attributes.getRand();
        auto receivedMac = receivedEap.attributes.getMac();
        auto receivedAutn = receivedEap.attributes.getAutn();

        if (receivedRand.length() != 16 || receivedAutn.length() != 16 || receivedMac.length() != 16)
        {
            sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
            return;
        }

        // =================================== Check the received KDF and KDF_INPUT ===================================

        if (receivedEap.attributes.getKdf() != 1)
        {
            m_logger->err("EAP AKA' Authentication Reject, received AT_KDF is not valid");
            if (networkFailingTheAuthCheck(true))
                return;
            m_timers->t3520.start();
            sendEapFailure(std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                              eap::ESubType::AKA_AUTHENTICATION_REJECT));
            return;
        }

        auto snn = keys::ConstructServingNetworkName(currentPlmn);

        if (receivedEap.attributes.getKdfInput() != OctetString::FromAscii(snn))
        {
            m_logger->err("EAP AKA' Authentication Reject, received AT_KDF_INPUT is not valid");

            sendEapFailure(std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                              eap::ESubType::AKA_AUTHENTICATION_REJECT));
            return;
        }

        // =================================== Check the received ngKSI ===================================

        if (msg.ngKSI.tsc == nas::ETypeOfSecurityContext::MAPPED_SECURITY_CONTEXT)
        {
            m_logger->err("Mapped security context not supported");
            sendAuthFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
            return;
        }

        if (msg.ngKSI.ksi == nas::IENasKeySetIdentifier::NOT_AVAILABLE_OR_RESERVED)
        {
            m_logger->err("Invalid ngKSI value received");
            sendAuthFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
            return;
        }

        if ((m_usim->m_currentNsCtx && m_usim->m_currentNsCtx->ngKsi == msg.ngKSI.ksi) ||
            (m_usim->m_nonCurrentNsCtx && m_usim->m_nonCurrentNsCtx->ngKsi == msg.ngKSI.ksi))
        {
            if (networkFailingTheAuthCheck(true))
                return;

            m_timers->t3520.start();
            sendAuthFailure(nas::EMmCause::NGKSI_ALREADY_IN_USE);
            return;
        }

        // =================================== Check the received AUTN ===================================

        auto autnCheck = validateAutn(receivedRand, receivedAutn);
        m_timers->t3516.start();

        if (autnCheck == EAutnValidationRes::OK)
        {
            // Calculate milenage
            auto milenage = calculateMilenage(m_usim->m_sqnMng->getSqn(), receivedRand, false);
            auto sqnXorAk = OctetString::Xor(m_usim->m_sqnMng->getSqn(), milenage.ak);
            auto ckPrimeIkPrime = keys::CalculateCkPrimeIkPrime(milenage.ck, milenage.ik, snn, sqnXorAk);
            auto &ckPrime = ckPrimeIkPrime.first;
            auto &ikPrime = ckPrimeIkPrime.second;

            auto mk = keys::CalculateMk(ckPrime, ikPrime, m_base->config->supi.value());
            auto kaut = mk.subCopy(16, 32);

            // Check the received AT_MAC
            auto expectedMac = keys::CalculateMacForEapAkaPrime(kaut, receivedEap);
            if (expectedMac != receivedMac)
            {
                m_logger->err("AT_MAC failure in EAP AKA'. expected: %s received: %s",
                              expectedMac.toHexString().c_str(), receivedMac.toHexString().c_str());
                if (networkFailingTheAuthCheck(true))
                    return;
                m_timers->t3520.start();

                auto eap = std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                              eap::ESubType::AKA_CLIENT_ERROR);
                eap->attributes.putClientErrorCode(0);
                sendEapFailure(std::move(eap));
                return;
            }

            // Store the relevant parameters
            m_usim->m_rand = receivedRand.copy();
            m_usim->m_resStar = {};

            // Create new partial native NAS security context and continue with key derivation
            m_usim->m_nonCurrentNsCtx = std::make_unique<NasSecurityContext>();
            m_usim->m_nonCurrentNsCtx->tsc = msg.ngKSI.tsc;
            m_usim->m_nonCurrentNsCtx->ngKsi = msg.ngKSI.ksi;
            m_usim->m_nonCurrentNsCtx->keys.kAusf = keys::CalculateKAusfForEapAkaPrime(mk);
            m_usim->m_nonCurrentNsCtx->keys.abba = msg.abba.rawData.copy();

            keys::DeriveKeysSeafAmf(*m_base->config, currentPlmn, *m_usim->m_nonCurrentNsCtx);

            // Send response
            m_nwConsecutiveAuthFailure = 0;
            m_timers->t3520.stop();
            {
                auto *akaPrimeResponse =
                    new eap::EapAkaPrime(eap::ECode::RESPONSE, receivedEap.id, eap::ESubType::AKA_CHALLENGE);
                akaPrimeResponse->attributes.putRes(milenage.res);
                akaPrimeResponse->attributes.putMac(OctetString::FromSpare(16)); // Dummy mac
                akaPrimeResponse->attributes.putKdf(1);

                // Calculate and put mac value
                auto sendingMac = keys::CalculateMacForEapAkaPrime(kaut, *akaPrimeResponse);
                akaPrimeResponse->attributes.replaceMac(sendingMac);

                nas::AuthenticationResponse resp;
                resp.eapMessage = nas::IEEapMessage{};
                resp.eapMessage->eap = std::unique_ptr<eap::EapAkaPrime>(akaPrimeResponse);

                sendNasMessage(resp);
            }
        }
        else if (autnCheck == EAutnValidationRes::MAC_FAILURE)
        {
            if (networkFailingTheAuthCheck(true))
                return;
            m_timers->t3520.start();
            sendEapFailure(std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                              eap::ESubType::AKA_AUTHENTICATION_REJECT));
        }
        else if (autnCheck == EAutnValidationRes::SYNCHRONISATION_FAILURE)
        {
            if (networkFailingTheAuthCheck(true))
                return;

            m_timers->t3520.start();

            auto milenage = calculateMilenage(m_usim->m_sqnMng->getSqn(), receivedRand, true);
            auto auts = keys::CalculateAuts(m_usim->m_sqnMng->getSqn(), milenage.ak_r, milenage.mac_s);

            auto eap = std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                          eap::ESubType::AKA_SYNCHRONIZATION_FAILURE);
            eap->attributes.putAuts(std::move(auts));
            sendEapFailure(std::move(eap));
        }
        else // the other case, separation bit mismatched
        {
            if (networkFailingTheAuthCheck(true))
                return;
            m_timers->t3520.start();

            auto eap = std::make_unique<eap::EapAkaPrime>(eap::ECode::RESPONSE, receivedEap.id,
                                                          eap::ESubType::AKA_CLIENT_ERROR);
            eap->attributes.putClientErrorCode(0);
            sendEapFailure(std::move(eap));
        }
    }
    else if (msg.eapMessage->eap->eapType == eap::EEapType::EAP_TLS)
    {
        auto &receivedEap = (const eap::EapTLS &)*msg.eapMessage->eap;
        // =================================== Check the received ngKSI ===================================

        if (msg.ngKSI.tsc == nas::ETypeOfSecurityContext::MAPPED_SECURITY_CONTEXT)
        {
            m_logger->err("Mapped security context not supported");
            sendAuthFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
            return;
        }

        if (msg.ngKSI.ksi == nas::IENasKeySetIdentifier::NOT_AVAILABLE_OR_RESERVED)
        {
            m_logger->err("Invalid ngKSI value received");
            sendAuthFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
            return;
        }
        // Handshake checking

        auto checkHandshakeState = [](SSL *ssl, int ret) {
            if (ret != 1)
            {
                int err = SSL_get_error(ssl, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    return true;
                return false;
            }
            return true;
        };

        if (m_tlsState == ETlsState::TLS_START)
        {
            if ((receivedEap.flag & 32) == 0)
            {
                sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
                return;
            }
            m_ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, NULL);
            SSL_CTX_load_verify_file(m_ctx, m_base->config->caCertificate.c_str());
            SSL_CTX_use_certificate_file(m_ctx, m_base->config->clientCertificate.c_str(), SSL_FILETYPE_PEM);

            auto ui = makeMethod();
            auto storeHandle = OSSL_STORE_open(m_base->config->clientPrivateKey.c_str(), ui,
                                               (void *)(&m_base->config->clientPassword), nullptr, nullptr);
            auto storeInfo = OSSL_STORE_load(storeHandle);
            m_pkey = OSSL_STORE_INFO_get1_PKEY(storeInfo);
            SSL_CTX_use_PrivateKey(m_ctx, m_pkey);
            OSSL_STORE_INFO_free(storeInfo);
            OSSL_STORE_close(storeHandle);
            UI_destroy_method(ui);
            m_ssl = SSL_new(m_ctx);
            m_rbio = BIO_new(BIO_s_mem());
            m_wbio = BIO_new(BIO_s_mem());
            SSL_set_bio(m_ssl, m_rbio, m_wbio);
            SSL_set_connect_state(m_ssl);
            m_tlsState = ETlsState::TLS_HANDSHAKE;
        }
        BIO_reset(m_rbio);
        BIO_write(m_rbio, receivedEap.tlsData.data(), receivedEap.tlsData.length());
        // TODO
        if (m_tlsState == ETlsState::TLS_HANDSHAKE)
        {
            BIO_reset(m_wbio);
            int state = SSL_do_handshake(m_ssl);
            if (state == 1)
            {
                m_timers->t3520.stop();
                m_tlsState = ETlsState::TLS_DONE;
                nas::AuthenticationResponse resp;
                resp.eapMessage = nas::IEEapMessage{};
                resp.eapMessage->eap =
                    std::make_unique<eap::EapTLS>(eap::ECode::RESPONSE, receivedEap.id, 128, OctetString::Empty());
                uint8_t keyMaterial[128];
                constexpr char *label = "client EAP encryption";
                SSL_export_keying_material(m_ssl, keyMaterial, sizeof(keyMaterial), label,
                                           std::char_traits<char>::length(label), nullptr, 0, 0);

                m_usim->m_nonCurrentNsCtx = std::make_unique<NasSecurityContext>();
                m_usim->m_nonCurrentNsCtx->tsc = msg.ngKSI.tsc;
                m_usim->m_nonCurrentNsCtx->ngKsi = msg.ngKSI.ksi;
                m_usim->m_nonCurrentNsCtx->keys.kAusf = OctetString::FromArray((uint8_t *)(keyMaterial + 64), 32);
                m_usim->m_nonCurrentNsCtx->keys.abba = msg.abba.rawData.copy();

                keys::DeriveKeysSeafAmf(*m_base->config, currentPlmn, *m_usim->m_nonCurrentNsCtx);
                sendNasMessage(resp);
                return;
            }
            if (!checkHandshakeState(m_ssl, state))
            {
                sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
                return;
            }
            char *data = nullptr;
            long dataSize = BIO_get_mem_data(m_wbio, &data);
            auto tlsResponse = std::make_unique<eap::EapTLS>(eap::ECode::RESPONSE, receivedEap.id, 128,
                                                             OctetString::FromArray((uint8_t *)data, dataSize));

            nas::AuthenticationResponse resp;
            resp.eapMessage = nas::IEEapMessage{};
            resp.eapMessage->eap = std::move(tlsResponse);
            sendNasMessage(resp);
            return;
        }
        if (m_tlsState == ETlsState::TLS_DONE)
        {
            EVP_PKEY_free(m_pkey);
            m_pkey = nullptr;
            BIO_free(m_wbio);
            BIO_free(m_rbio);
            m_wbio = m_rbio = nullptr;
            SSL_free(m_ssl);
            m_ssl = nullptr;
            SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
            return;
        }
    }
    else
    {
        sendMmStatus(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
        return;
    }
}

void NasMm::receiveAuthenticationRequest5gAka(const nas::AuthenticationRequest &msg)
{
    Plmn currentPLmn = m_base->shCtx.getCurrentPlmn();
    if (!currentPLmn.hasValue())
        return;

    auto sendFailure = [this](nas::EMmCause cause, std::optional<OctetString> &&auts = std::nullopt) {
        if (cause != nas::EMmCause::SYNCH_FAILURE)
            m_logger->err("Sending Authentication Failure with cause [%s]", nas::utils::EnumToString(cause));
        else
            m_logger->debug("Sending Authentication Failure due to SQN out of range");

        // Clear RAND and RES* stored in volatile memory
        m_usim->m_rand = {};
        m_usim->m_resStar = {};

        // Stop T3516 if running
        m_timers->t3516.stop();

        // Send Authentication Failure
        nas::AuthenticationFailure resp{};
        resp.mmCause.value = cause;

        if (auts.has_value())
        {
            resp.authenticationFailureParameter = nas::IEAuthenticationFailureParameter{};
            resp.authenticationFailureParameter->rawData = std::move(*auts);
        }

        sendNasMessage(resp);
    };

    // ========================== Check the received parameters syntactically ==========================

    if (!msg.authParamRAND.has_value() || !msg.authParamAUTN.has_value())
    {
        sendFailure(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
        return;
    }

    if (msg.authParamRAND->value.length() != 16 || msg.authParamAUTN->value.length() != 16)
    {
        sendFailure(nas::EMmCause::SEMANTICALLY_INCORRECT_MESSAGE);
        return;
    }

    // =================================== Check the received ngKSI ===================================

    if (msg.ngKSI.tsc == nas::ETypeOfSecurityContext::MAPPED_SECURITY_CONTEXT)
    {
        m_logger->err("Mapped security context not supported");
        sendFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
        return;
    }

    if (msg.ngKSI.ksi == nas::IENasKeySetIdentifier::NOT_AVAILABLE_OR_RESERVED)
    {
        m_logger->err("Invalid ngKSI value received");
        sendFailure(nas::EMmCause::UNSPECIFIED_PROTOCOL_ERROR);
        return;
    }

    if ((m_usim->m_currentNsCtx && m_usim->m_currentNsCtx->ngKsi == msg.ngKSI.ksi) ||
        (m_usim->m_nonCurrentNsCtx && m_usim->m_nonCurrentNsCtx->ngKsi == msg.ngKSI.ksi))
    {
        if (networkFailingTheAuthCheck(true))
            return;

        m_timers->t3520.start();
        sendFailure(nas::EMmCause::NGKSI_ALREADY_IN_USE);
        return;
    }

    // ============================================ Others ============================================

    auto &rand = msg.authParamRAND->value;
    auto &autn = msg.authParamAUTN->value;

    EAutnValidationRes autnCheck = EAutnValidationRes::OK;

    // If the received RAND is same with store stored RAND, bypass AUTN validation
    // NOTE: Not completely sure if this is correct and the spec meant this. But in worst case, synchronisation failure
    //  happens, and hopefully that can be restored with the normal resynchronization procedure.
    if (m_usim->m_rand != rand)
    {
        autnCheck = validateAutn(rand, autn);
        m_timers->t3516.start();
    }

    if (autnCheck == EAutnValidationRes::OK)
    {
        // Calculate milenage
        auto milenage = calculateMilenage(m_usim->m_sqnMng->getSqn(), rand, false);
        auto ckIk = OctetString::Concat(milenage.ck, milenage.ik);
        auto sqnXorAk = OctetString::Xor(m_usim->m_sqnMng->getSqn(), milenage.ak);
        auto snn = keys::ConstructServingNetworkName(currentPLmn);

        // Store the relevant parameters
        m_usim->m_rand = rand.copy();
        m_usim->m_resStar = keys::CalculateResStar(ckIk, snn, rand, milenage.res);

        // Create new partial native NAS security context and continue with key derivation
        m_usim->m_nonCurrentNsCtx = std::make_unique<NasSecurityContext>();
        m_usim->m_nonCurrentNsCtx->tsc = msg.ngKSI.tsc;
        m_usim->m_nonCurrentNsCtx->ngKsi = msg.ngKSI.ksi;
        m_usim->m_nonCurrentNsCtx->keys.kAusf = keys::CalculateKAusfFor5gAka(milenage.ck, milenage.ik, snn, sqnXorAk);
        m_usim->m_nonCurrentNsCtx->keys.abba = msg.abba.rawData.copy();

        keys::DeriveKeysSeafAmf(*m_base->config, currentPLmn, *m_usim->m_nonCurrentNsCtx);

        // Send response
        m_nwConsecutiveAuthFailure = 0;
        m_timers->t3520.stop();

        nas::AuthenticationResponse resp;
        resp.authenticationResponseParameter = nas::IEAuthenticationResponseParameter{};
        resp.authenticationResponseParameter->rawData = m_usim->m_resStar.copy();

        sendNasMessage(resp);
    }
    else if (autnCheck == EAutnValidationRes::MAC_FAILURE)
    {
        if (networkFailingTheAuthCheck(true))
            return;
        m_timers->t3520.start();
        sendFailure(nas::EMmCause::MAC_FAILURE);
    }
    else if (autnCheck == EAutnValidationRes::SYNCHRONISATION_FAILURE)
    {
        if (networkFailingTheAuthCheck(true))
            return;

        m_timers->t3520.start();

        auto milenage = calculateMilenage(m_usim->m_sqnMng->getSqn(), rand, true);
        auto auts = keys::CalculateAuts(m_usim->m_sqnMng->getSqn(), milenage.ak_r, milenage.mac_s);
        sendFailure(nas::EMmCause::SYNCH_FAILURE, std::move(auts));
    }
    else // the other case, separation bit mismatched
    {
        if (networkFailingTheAuthCheck(true))
            return;
        m_timers->t3520.start();
        sendFailure(nas::EMmCause::NON_5G_AUTHENTICATION_UNACCEPTABLE);
    }
}

void NasMm::receiveAuthenticationResult(const nas::AuthenticationResult &msg)
{
    if (msg.abba.has_value())
        m_usim->m_nonCurrentNsCtx->keys.abba = msg.abba->rawData.copy();

    if (msg.eapMessage.eap->code == eap::ECode::SUCCESS)
        receiveEapSuccessMessage(*msg.eapMessage.eap);
    else if (msg.eapMessage.eap->code == eap::ECode::FAILURE)
        receiveEapFailureMessage(*msg.eapMessage.eap);
    else
        m_logger->warn("Network sent EAP with an inconvenient type in Authentication Result, ignoring EAP IE.");
}

void NasMm::receiveAuthenticationReject(const nas::AuthenticationReject &msg)
{
    m_logger->err("Authentication Reject received");

    // The RAND and RES* values stored in the ME shall be deleted and timer T3516, if running, shall be stopped
    m_usim->m_rand = {};
    m_usim->m_resStar = {};
    m_timers->t3516.stop();

    if (msg.eapMessage.has_value())
    {
        if (msg.eapMessage->eap->code == eap::ECode::FAILURE)
            receiveEapFailureMessage(*msg.eapMessage->eap);
        else
            m_logger->warn("Network sent EAP with inconvenient type in AuthenticationReject, ignoring EAP IE.");
    }

    // The UE shall set the update status to 5U3 ROAMING NOT ALLOWED,
    switchUState(E5UState::U3_ROAMING_NOT_ALLOWED);
    // Delete the stored 5G-GUTI, TAI list, last visited registered TAI and ngKSI. The USIM shall be considered invalid
    // until switching off the UE or the UICC containing the USIM is removed
    m_storage->storedGuti->clear();
    m_storage->lastVisitedRegisteredTai->clear();
    m_storage->taiList->clear();
    m_usim->m_currentNsCtx = {};
    m_usim->m_nonCurrentNsCtx = {};
    m_usim->invalidate();
    // The UE shall abort any 5GMM signalling procedure, stop any of the timers T3510, T3516, T3517, T3519 or T3521 (if
    // they were running) ..
    m_timers->t3510.stop();
    m_timers->t3516.stop();
    m_timers->t3517.stop();
    m_timers->t3519.stop();
    m_timers->t3521.stop();
    // .. and enter state 5GMM-DEREGISTERED.
    switchMmState(EMmSubState::MM_DEREGISTERED_PS);
}

void NasMm::receiveEapSuccessMessage(const eap::Eap &eap)
{
    // do nothing
}

void NasMm::receiveEapFailureMessage(const eap::Eap &eap)
{
    m_logger->debug("Handling EAP-failure");

    // UE shall delete the partial native 5G NAS security context if any was created
    m_usim->m_nonCurrentNsCtx = {};
}

EAutnValidationRes NasMm::validateAutn(const OctetString &rand, const OctetString &autn)
{
    // Decode AUTN
    OctetString receivedSQNxorAK = autn.subCopy(0, 6);
    OctetString receivedAMF = autn.subCopy(6, 2);
    OctetString receivedMAC = autn.subCopy(8, 8);

    // Check the separation bit
    if (receivedAMF.get(0).bit(7) != 1)
    {
        m_logger->err("AUTN validation SEP-BIT failure. expected: 1, received: 0");
        return EAutnValidationRes::AMF_SEPARATION_BIT_FAILURE;
    }

    // Derive AK and MAC
    auto milenage = calculateMilenage(m_usim->m_sqnMng->getSqn(), rand, false);
    OctetString receivedSQN = OctetString::Xor(receivedSQNxorAK, milenage.ak);

    m_logger->debug("Received SQN [%s]", receivedSQN.toHexString().c_str());
    m_logger->debug("SQN-MS [%s]", m_usim->m_sqnMng->getSqn().toHexString().c_str());

    // Verify that the received sequence number SQN is in the correct range
    bool sqn_ok = m_usim->m_sqnMng->checkSqn(receivedSQN);

    // Re-execute the milenage calculation (if case of sqn is changed with the received value)
    milenage = calculateMilenage(receivedSQN, rand, false);

    // Check MAC
    if (receivedMAC != milenage.mac_a)
    {
        m_logger->err("AUTN validation MAC mismatch. expected [%s] received [%s]", milenage.mac_a.toHexString().c_str(),
                      receivedMAC.toHexString().c_str());
        return EAutnValidationRes::MAC_FAILURE;
    }

    if (!sqn_ok)
        return EAutnValidationRes::SYNCHRONISATION_FAILURE;

    return EAutnValidationRes::OK;
}

crypto::milenage::Milenage NasMm::calculateMilenage(const OctetString &sqn, const OctetString &rand, bool dummyAmf)
{
    OctetString amf = dummyAmf ? OctetString::FromSpare(2) : m_base->config->amf.copy();

    if (m_base->config->opType == OpType::OPC)
        return crypto::milenage::Calculate(m_base->config->opC, m_base->config->key, rand, sqn, amf);

    OctetString opc = crypto::milenage::CalculateOpC(m_base->config->opC, m_base->config->key);
    return crypto::milenage::Calculate(opc, m_base->config->key, rand, sqn, amf);
}

bool NasMm::networkFailingTheAuthCheck(bool hasChance)
{
    if (hasChance && m_nwConsecutiveAuthFailure++ < 3)
        return false;

    // NOTE: Normally if we should check if the UE has an emergency. If it has, it should consider as network passed the
    //  auth check, instead of performing the actions in the following lines. But it's difficult to maintain and
    //  implement this behaviour. Therefore we would expect other solutions for an emergency case. Such as
    //  - Network initiates a Security Mode Command with IA0 and EA0
    //  - UE performs emergency registration after releasing the connection
    // END

    m_logger->err("Network failing the authentication check");

    if (m_cmState == ECmState::CM_CONNECTED)
        localReleaseConnection(true);

    m_timers->t3520.stop();
    return true;
}

} // namespace nr::ue
