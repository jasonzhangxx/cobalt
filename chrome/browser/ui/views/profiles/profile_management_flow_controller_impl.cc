// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/profiles/profile_management_flow_controller_impl.h"

#include <memory>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/signin_util.h"
#include "chrome/browser/ui/views/profiles/profile_management_step_controller.h"
#include "chrome/browser/ui/views/profiles/profile_management_types.h"
#include "chrome/browser/ui/views/profiles/profile_picker_signed_in_flow_controller.h"
#include "content/public/browser/web_contents.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/skia/include/core/SkColor.h"

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
#include "chrome/browser/ui/views/profiles/profile_picker_dice_sign_in_provider.h"
#endif

ProfileManagementFlowControllerImpl::ProfileManagementFlowControllerImpl(
    ProfilePickerWebContentsHost* host,
    ClearHostClosure clear_host_callback)
    : ProfileManagementFlowController(host, std::move(clear_host_callback)) {}

ProfileManagementFlowControllerImpl::~ProfileManagementFlowControllerImpl() =
    default;

void ProfileManagementFlowControllerImpl::SwitchToIdentityStepsFromPostSignIn(
    Profile* signed_in_profile,
    std::unique_ptr<content::WebContents> contents,
    StepSwitchFinishedCallback step_switch_finished_callback) {
  DCHECK_NE(Step::kPostSignInFlow, current_step());
  DCHECK(!IsStepInitialized(Step::kPostSignInFlow));
  RegisterStep(Step::kPostSignInFlow,
               CreatePostSignInStep(signed_in_profile, std::move(contents)));
  SwitchToStep(Step::kPostSignInFlow,
               /*reset_state=*/true, std::move(step_switch_finished_callback));
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
void ProfileManagementFlowControllerImpl::
    SwitchToIdentityStepsFromAccountSelection(
        StepSwitchFinishedCallback step_switch_finished_callback) {
  DCHECK_NE(Step::kAccountSelection, current_step());
  DCHECK_NE(Step::kPostSignInFlow, current_step());

  bool step_needs_registration = !IsStepInitialized(Step::kAccountSelection);
  if (step_needs_registration) {
    RegisterStep(
        Step::kAccountSelection,
        ProfileManagementStepController::CreateForDiceSignIn(
            host(), CreateDiceSignInProvider(),
            base::BindOnce(
                &ProfileManagementFlowControllerImpl::HandleSignInCompleted,
                // Binding as Unretained as `this`
                // outlives the step controllers.
                base::Unretained(this))));
  }
  auto pop_closure = base::BindOnce(
      &ProfileManagementFlowControllerImpl::SwitchToStep,
      // Binding as Unretained as `this` outlives the step
      // controllers.
      base::Unretained(this), current_step(),
      /*reset_state=*/false,
      /*step_switch_finished_callback=*/StepSwitchFinishedCallback(),
      /*pop_step_callback=*/base::OnceClosure());
  SwitchToStep(Step::kAccountSelection,
               /*reset_state=*/step_needs_registration,
               std::move(step_switch_finished_callback),
               std::move(pop_closure));
}

absl::optional<SkColor> ProfileManagementFlowControllerImpl::GetProfileColor() {
  return absl::nullopt;
}
#endif

std::unique_ptr<ProfileManagementStepController>
ProfileManagementFlowControllerImpl::CreatePostSignInStep(
    Profile* signed_in_profile,
    std::unique_ptr<content::WebContents> contents) {
  auto finish_flow_callback = FinishFlowCallback(base::BindOnce(
      &ProfileManagementFlowControllerImpl::FinishFlowAndRunInBrowser,
      // Unretained ok: the callback is passed to a step that
      // the `this` will own and outlive.
      base::Unretained(this),
      // Unretained ok: the steps register a profile alive and
      // will be alive until this callback runs.
      base::Unretained(signed_in_profile)));
  return ProfileManagementStepController::CreateForPostSignInFlow(
      host(),
      CreateSignedInFlowController(signed_in_profile, std::move(contents),
                                   std::move(finish_flow_callback)));
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
std::unique_ptr<ProfileManagementStepController>
ProfileManagementFlowControllerImpl::CreateSamlStep(
    Profile* signed_in_profile,
    std::unique_ptr<content::WebContents> contents) {
  auto finish_flow_callback = FinishFlowCallback(base::BindOnce(
      &ProfileManagementFlowControllerImpl::FinishFlowAndRunInBrowser,
      // Unretained ok: the callback is passed to a step that
      // the `flow_controller_` will own and outlive.
      base::Unretained(this),
      // Unretained ok: the steps register a profile alive and
      // will be alive until this callback runs.
      base::Unretained(signed_in_profile)));

  return ProfileManagementStepController::CreateForFinishSamlSignIn(
      host(), signed_in_profile, std::move(contents), GetProfileColor(),
      std::move(finish_flow_callback));
}

void ProfileManagementFlowControllerImpl::HandleSignInCompleted(
    Profile* signed_in_profile,
    bool is_saml,
    std::unique_ptr<content::WebContents> contents) {
  DCHECK(!signin_util::IsForceSigninEnabled());
  DCHECK(signed_in_profile);
  DCHECK_EQ(Step::kAccountSelection, current_step());

  Step step;
  if (is_saml) {
    step = Step::kFinishSamlSignin;
    DCHECK(!IsStepInitialized(step));
    // The SAML step controller handles finishing the profile setup by itself
    // when we switch to it.
    RegisterStep(step, CreateSamlStep(signed_in_profile, std::move(contents)));
  } else {
    step = Step::kPostSignInFlow;
    DCHECK(!IsStepInitialized(step));
    RegisterStep(step,
                 CreatePostSignInStep(signed_in_profile, std::move(contents)));
  }

  SwitchToStep(step, /*reset_state=*/true);

  // If we need to go back, we should go all the way to the beginning of the
  // flow and after that, recreate the account selection step to ensure no data
  // leaks if we select a different account.
  // We also erase the step after the switch here because it holds a
  // `ScopedProfileKeepAlive` and we need the next step to register its own
  // before this the account selection's is released.
  UnregisterStep(Step::kAccountSelection);
}
#endif
