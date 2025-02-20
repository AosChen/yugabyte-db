/*
 * Copyright 2021 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.controllers;

import static com.yugabyte.yw.forms.PlatformResults.YBPSuccess.empty;

import com.google.inject.Inject;
import com.yugabyte.yw.commissioner.Common.CloudType;
import com.yugabyte.yw.commissioner.tasks.UpdateLoadBalancerConfig;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.controllers.handlers.UniverseActionsHandler;
import com.yugabyte.yw.forms.AlertConfigFormData;
import com.yugabyte.yw.forms.EncryptionAtRestKeyParams;
import com.yugabyte.yw.forms.PlatformResults;
import com.yugabyte.yw.forms.PlatformResults.YBPSuccess;
import com.yugabyte.yw.forms.PlatformResults.YBPTask;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseResp;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import io.swagger.annotations.Api;
import io.swagger.annotations.ApiOperation;
import io.swagger.annotations.Authorization;
import java.io.IOException;
import java.util.List;
import java.util.UUID;
import lombok.extern.slf4j.Slf4j;
import play.mvc.Http;
import play.mvc.Http.Request;
import play.mvc.Result;

@Api(
    value = "Universe management",
    authorizations = @Authorization(AbstractPlatformController.API_KEY_AUTH))
@Slf4j
public class UniverseActionsController extends AuthenticatedController {
  @Inject private UniverseActionsHandler universeActionsHandler;
  @Inject private RuntimeConfigFactory runtimeConfigFactory;

  @ApiOperation(
      value = "Configure alerts for a universe",
      nickname = "configureUniverseAlerts",
      response = YBPSuccess.class)
  public Result configureAlerts(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);
    universeActionsHandler.configureAlerts(
        universe, formFactory.getFormDataOrBadRequest(request, AlertConfigFormData.class));
    auditService()
        .createAuditEntryWithReqBody(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.ConfigUniverseAlert);
    return empty();
  }

  @ApiOperation(value = "Pause a universe", nickname = "pauseUniverse", response = YBPTask.class)
  public Result pause(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);
    // Check if the universe is of type kubernetes, if yes throw an exception
    Cluster cluster = universe.getUniverseDetails().getPrimaryCluster();
    List<Cluster> readOnlyClusters = universe.getUniverseDetails().getReadOnlyClusters();
    CloudType cloudType = cluster.userIntent.providerType;
    boolean isKubernetesCluster = (cloudType == CloudType.kubernetes);
    for (Cluster readCluster : readOnlyClusters) {
      cloudType = readCluster.userIntent.providerType;
      isKubernetesCluster = isKubernetesCluster || cloudType == CloudType.kubernetes;
    }

    if (isKubernetesCluster) {
      String msg =
          String.format(
              "Pause task is not supported for Kubernetes universe - %s", universe.getName());
      log.error(msg);
      throw new IllegalArgumentException(msg);
    }

    UUID taskUUID = universeActionsHandler.pause(customer, universe);
    auditService()
        .createAuditEntry(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.Pause,
            taskUUID);
    return new YBPTask(taskUUID, universe.getUniverseUUID()).asResult();
  }

  @ApiOperation(
      value = "Resume a paused universe",
      nickname = "resumeUniverse",
      response = YBPTask.class)
  public Result resume(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);

    UUID taskUUID;
    try {
      taskUUID = universeActionsHandler.resume(customer, universe);
    } catch (IOException e) {
      log.error(e.getMessage(), e);
      throw new PlatformServiceException(INTERNAL_SERVER_ERROR, e.getMessage());
    }

    auditService()
        .createAuditEntry(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.Resume,
            taskUUID);
    return new YBPTask(taskUUID, universe.getUniverseUUID()).asResult();
  }

  @ApiOperation(
      value = "Set a universe's key",
      nickname = "setUniverseKey",
      response = UniverseResp.class)
  public Result setUniverseKey(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);

    log.info("Updating universe key {} for {}.", universe.getUniverseUUID(), customer.getUuid());
    // Get the user submitted form data.

    EncryptionAtRestKeyParams taskParams =
        EncryptionAtRestKeyParams.bindFromFormData(universe.getUniverseUUID(), request);

    UUID taskUUID = universeActionsHandler.setUniverseKey(customer, universe, taskParams);

    auditService()
        .createAuditEntryWithReqBody(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.SetUniverseKey,
            taskUUID);
    UniverseResp resp =
        UniverseResp.create(universe, taskUUID, runtimeConfigFactory.globalRuntimeConf());
    return PlatformResults.withData(resp);
  }

  @ApiOperation(
      value = "Update load balancer config",
      nickname = "updateLoadBalancerConfig",
      response = UpdateLoadBalancerConfig.class)
  public Result updateLoadBalancerConfig(
      UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);

    log.info(
        "Updating load balancer config {} for {}.", universe.getUniverseUUID(), customer.getUuid());

    UUID taskUUID =
        universeActionsHandler.updateLoadBalancerConfig(
            customer,
            universe,
            formFactory.getFormDataOrBadRequest(
                request.body().asJson(), UniverseDefinitionTaskParams.class));
    auditService()
        .createAuditEntryWithReqBody(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.UpdateLoadBalancerConfig,
            taskUUID);
    return new YBPTask(taskUUID, universe.getUniverseUUID()).asResult();
  }

  /**
   * Mark whether the universe needs to be backed up or not.
   *
   * @return Result
   */
  @ApiOperation(
      value = "Set a universe's backup flag",
      nickname = "setUniverseBackupFlag",
      tags = {"Universe management", "Backups"},
      response = YBPSuccess.class)
  public Result setBackupFlag(
      UUID customerUUID, UUID universeUUID, Boolean markActive, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);

    universeActionsHandler.setBackupFlag(universe, markActive);
    auditService()
        .createAuditEntry(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.SetBackupFlag);
    return empty();
  }

  /**
   * Mark whether the universe has been made helm compatible.
   *
   * @return Result
   */
  @ApiOperation(
      value = "Flag a universe as Helm 3-compatible",
      nickname = "setUniverseHelm3Compatible",
      response = YBPSuccess.class)
  public Result setHelm3Compatible(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);
    universeActionsHandler.setHelm3Compatible(universe);
    auditService()
        .createAuditEntry(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.SetHelm3Compatible);
    return empty();
  }

  /**
   * API that sets universe version number to -1
   *
   * @return result of settings universe version to -1 (either success if universe exists else
   *     failure
   */
  @ApiOperation(
      value = "Reset universe version",
      nickname = "resetUniverseVersion",
      response = YBPSuccess.class)
  public Result resetVersion(UUID customerUUID, UUID universeUUID, Http.Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);
    auditService()
        .createAuditEntry(
            request,
            Audit.TargetType.Universe,
            universeUUID.toString(),
            Audit.ActionType.ResetUniverseVersion);
    universe.resetVersion();
    return empty();
  }

  @ApiOperation(
      hidden = true,
      value = "Unlock a universe",
      notes = "Unlock a universe",
      response = YBPSuccess.class)
  public Result unlockUniverse(UUID customerUUID, UUID universeUUID, Request request) {
    Customer customer = Customer.getOrBadRequest(customerUUID);
    Universe universe = Universe.getOrBadRequest(universeUUID, customer);
    Util.unlockUniverse(universe);
    auditService()
        .createAuditEntry(
            request, Audit.TargetType.Universe, universeUUID.toString(), Audit.ActionType.Unlock);
    return empty();
  }
}
