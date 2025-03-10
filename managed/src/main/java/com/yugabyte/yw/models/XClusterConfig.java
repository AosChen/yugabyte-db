// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import static play.mvc.Http.Status.BAD_REQUEST;

import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.collect.BiMap;
import com.google.common.collect.ImmutableBiMap;
import com.yugabyte.yw.commissioner.tasks.XClusterConfigTaskBase;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.forms.XClusterConfigCreateFormData;
import io.ebean.Finder;
import io.ebean.Model;
import io.ebean.annotation.DbEnumValue;
import io.ebean.annotation.Transactional;
import io.swagger.annotations.ApiModel;
import io.swagger.annotations.ApiModelProperty;
import java.util.Collection;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;
import java.util.stream.Stream;
import javax.annotation.Nullable;
import javax.persistence.CascadeType;
import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.JoinColumn;
import javax.persistence.ManyToOne;
import javax.persistence.OneToMany;
import lombok.Getter;
import lombok.Setter;
import lombok.extern.slf4j.Slf4j;
import org.apache.commons.collections4.CollectionUtils;
import org.yb.CommonTypes;
import org.yb.master.MasterDdlOuterClass;

@Slf4j
@Entity
@ApiModel(description = "xcluster config object")
@Getter
@Setter
public class XClusterConfig extends Model {

  public static final BiMap<TableType, CommonTypes.TableType>
      XClusterConfigTableTypeCommonTypesTableTypeBiMap =
          ImmutableBiMap.of(
              TableType.YSQL,
              CommonTypes.TableType.PGSQL_TABLE_TYPE,
              TableType.YCQL,
              CommonTypes.TableType.YQL_TABLE_TYPE);

  private static final Finder<UUID, XClusterConfig> find =
      new Finder<UUID, XClusterConfig>(XClusterConfig.class) {};

  @Id
  @ApiModelProperty(value = "XCluster config UUID")
  private UUID uuid;

  @Column(name = "config_name")
  @ApiModelProperty(value = "XCluster config name")
  private String name;

  @ManyToOne
  @JoinColumn(name = "source_universe_uuid", referencedColumnName = "universe_uuid")
  @ApiModelProperty(value = "Source Universe UUID")
  private UUID sourceUniverseUUID;

  @ManyToOne
  @JoinColumn(name = "target_universe_uuid", referencedColumnName = "universe_uuid")
  @ApiModelProperty(value = "Target Universe UUID")
  private UUID targetUniverseUUID;

  @ApiModelProperty(
      value = "Status",
      allowableValues = "Initialized, Running, Updating, DeletedUniverse, DeletionFailed, Failed")
  private XClusterConfigStatusType status;

  public enum XClusterConfigStatusType {
    Initialized("Initialized"),
    Running("Running"),
    Updating("Updating"),
    DeletedUniverse("DeletedUniverse"),
    DeletionFailed("DeletionFailed"),
    Failed("Failed");

    private final String status;

    XClusterConfigStatusType(String status) {
      this.status = status;
    }

    @Override
    @DbEnumValue
    public String toString() {
      return this.status;
    }
  }

  public enum TableType {
    UNKNOWN,
    YSQL,
    YCQL;

    @Override
    @DbEnumValue
    public String toString() {
      return super.toString();
    }
  }

  @ApiModelProperty(value = "tableType", allowableValues = "UNKNOWN, YSQL, YCQL")
  private TableType tableType;

  @ApiModelProperty(value = "Whether this xCluster replication config is paused")
  private boolean paused;

  @ApiModelProperty(value = "Whether this xCluster replication config was imported")
  private boolean imported;

  @ApiModelProperty(value = "Create time of the xCluster config", example = "2022-12-12T13:07:18Z")
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd'T'HH:mm:ss'Z'")
  private Date createTime;

  @ApiModelProperty(
      value = "Last modify time of the xCluster config",
      example = "2022-12-12T13:07:18Z")
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd'T'HH:mm:ss'Z'")
  private Date modifyTime;

  @OneToMany(mappedBy = "config", cascade = CascadeType.ALL, orphanRemoval = true)
  @ApiModelProperty(value = "Tables participating in this xCluster config")
  @JsonProperty("tableDetails")
  private Set<XClusterTableConfig> tables = new HashSet<>();

  @ApiModelProperty(value = "Replication group name in DB")
  private String replicationGroupName;

  public enum ConfigType {
    Basic,
    Txn;

    @Override
    @DbEnumValue
    public String toString() {
      return super.toString();
    }

    public static ConfigType getFromString(@Nullable String value) {
      if (Objects.isNull(value)) {
        return ConfigType.Basic; // Default value
      }
      return Enum.valueOf(ConfigType.class, value);
    }
  }

  @ApiModelProperty(value = "Whether the config is txn xCluster")
  private ConfigType type;

  @ApiModelProperty(value = "Whether the source is active in txn xCluster")
  private boolean sourceActive;

  @ApiModelProperty(value = "Whether the target is active in txn xCluster")
  private boolean targetActive;

  @Override
  public String toString() {
    return this.getReplicationGroupName()
        + "(uuid="
        + this.getUuid()
        + ",targetUuid="
        + this.getTargetUniverseUUID()
        + ",status="
        + this.getStatus()
        + ",paused="
        + this.isPaused()
        + ",tableType="
        + this.getTableType()
        + ",type="
        + this.getType()
        + ")";
  }

  public Optional<XClusterTableConfig> maybeGetTableById(String tableId) {
    // There will be at most one tableConfig for a tableId within each xCluster config.
    return this.getTableDetails().stream()
        .filter(tableConfig -> tableConfig.getTableId().equals(tableId))
        .findAny();
  }

  @JsonIgnore
  public CommonTypes.TableType getTableTypeAsCommonType() {
    if (getTableType().equals(TableType.UNKNOWN)) {
      throw new RuntimeException(
          "Table type is UNKNOWN, and cannot be mapped to CommonTypes.TableType");
    }
    return XClusterConfigTableTypeCommonTypesTableTypeBiMap.get(getTableType());
  }

  @JsonIgnore
  public CommonTypes.TableType updateTableType(
      List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> tableInfoList) {
    if (!this.getTableType().equals(TableType.UNKNOWN)) {
      log.info("tableType for {} is already set; skip setting it", this);
      return getTableTypeAsCommonType();
    }
    if (tableInfoList.isEmpty()) {
      log.warn(
          "tableType for {} is unknown and cannot be deducted from tableInfoList because "
              + "it is empty",
          this);
      return getTableTypeAsCommonType();
    }
    CommonTypes.TableType typeAsCommonType = tableInfoList.get(0).getTableType();
    // All tables have the same type.
    if (!tableInfoList.stream()
        .allMatch(tableInfo -> tableInfo.getTableType().equals(typeAsCommonType))) {
      throw new IllegalArgumentException(
          "At least one table has a different type from others. "
              + "All tables in an xCluster config must have the same type. Please create separate "
              + "xCluster configs for different table types.");
    }
    if (!XClusterConfigTableTypeCommonTypesTableTypeBiMap.containsValue(typeAsCommonType)) {
      throw new IllegalArgumentException(
          String.format(
              "Only %s supported as CommonTypes.TableType for xCluster replication; got %s",
              XClusterConfigTableTypeCommonTypesTableTypeBiMap.values(), typeAsCommonType));
    }
    this.setTableType(
        XClusterConfigTableTypeCommonTypesTableTypeBiMap.inverse().get(typeAsCommonType));
    update();
    return typeAsCommonType;
  }

  public XClusterTableConfig getTableById(String tableId) {
    Optional<XClusterTableConfig> tableConfig = maybeGetTableById(tableId);
    if (!tableConfig.isPresent()) {
      throw new IllegalArgumentException(
          String.format(
              "Table with id (%s) does not belong to the xClusterConfig %s", tableId, this));
    }
    return tableConfig.get();
  }

  public Set<XClusterTableConfig> getTablesById(Set<String> tableIds) {
    Map<String, XClusterTableConfig> tableConfigMap =
        this.getTableDetails().stream()
            .collect(
                Collectors.toMap(
                    tableConfig -> tableConfig.getTableId(), tableConfig -> tableConfig));
    Set<XClusterTableConfig> tableConfigs = new HashSet<>();
    tableIds.forEach(
        tableId -> {
          XClusterTableConfig tableConfig = tableConfigMap.get(tableId);
          if (tableConfig == null) {
            throw new IllegalArgumentException(
                String.format(
                    "Table with id (%s) does not belong to the xClusterConfig %s", tableId, this));
          }
          tableConfigs.add(tableConfig);
        });
    return tableConfigs;
  }

  @JsonIgnore
  public Set<XClusterTableConfig> getTableDetails() {
    return tables;
  }

  @JsonProperty("tables")
  public Set<String> getTableIds() {
    return this.tables.stream().map(XClusterTableConfig::getTableId).collect(Collectors.toSet());
  }

  @JsonIgnore
  public Set<String> getTableIdsWithReplicationSetup(Set<String> tableIds, boolean done) {
    return this.getTableDetails().stream()
        .filter(
            table ->
                tableIds.contains(table.getTableId()) && table.isReplicationSetupDone() == done)
        .map(table -> table.getTableId())
        .collect(Collectors.toSet());
  }

  @JsonIgnore
  public Set<String> getTableIdsWithReplicationSetup(boolean done) {
    return getTableIdsWithReplicationSetup(getTableIds(), done);
  }

  @JsonIgnore
  public Set<String> getTableIdsWithReplicationSetup() {
    return getTableIdsWithReplicationSetup(true /* done */);
  }

  @JsonIgnore
  public Set<String> getTableIds(boolean includeMainTables, boolean includeIndexTables) {
    if (!includeMainTables && !includeIndexTables) {
      throw new IllegalArgumentException(
          "Both includeMainTables and includeIndexTables cannot be false");
    }
    if (includeMainTables && includeIndexTables) {
      return this.getTableIds();
    }
    return this.getTables().stream()
        .filter(table -> table.isIndexTable() == includeIndexTables)
        .map(table -> table.getTableId())
        .collect(Collectors.toSet());
  }

  @JsonIgnore
  public Set<String> getTableIdsExcludeIndexTables() {
    return getTableIds(true /* includeMainTables */, false /* includeIndexTables */);
  }

  public void updateTables(Set<String> tableIds) {
    updateTables(tableIds, null /* tableIdsNeedBootstrap */);
  }

  @Transactional
  public void updateTables(Set<String> tableIds, Set<String> tableIdsNeedBootstrap) {
    this.getTables().clear();
    addTables(tableIds, tableIdsNeedBootstrap);
  }

  @Transactional
  public void addTables(Set<String> tableIds, Set<String> tableIdsNeedBootstrap) {
    if (tableIds == null) {
      throw new IllegalArgumentException("tableIds cannot be null");
    }
    // Ensure tableIdsNeedBootstrap is a subset of tableIds.
    if (tableIdsNeedBootstrap != null && !tableIds.containsAll(tableIdsNeedBootstrap)) {
      String errMsg =
          String.format(
              "The set of tables in tableIdsNeedBootstrap (%s) is not a subset of tableIds (%s)",
              tableIdsNeedBootstrap, tableIds);
      throw new IllegalArgumentException(errMsg);
    }
    tableIds.forEach(
        tableId -> {
          XClusterTableConfig tableConfig = new XClusterTableConfig(this, tableId);
          if (tableIdsNeedBootstrap != null && tableIdsNeedBootstrap.contains(tableId)) {
            tableConfig.setNeedBootstrap(true);
          }
          addTableConfig(tableConfig);
        });
    update();
  }

  @Transactional
  public void addTablesIfNotExist(
      Set<String> tableIds, Set<String> tableIdsNeedBootstrap, boolean areIndexTables) {
    if (tableIds.isEmpty()) {
      return;
    }
    Set<String> nonExistingTableIds =
        tableIds.stream()
            .filter(tableId -> !this.getTableIds().contains(tableId))
            .collect(Collectors.toSet());
    Set<String> nonExistingTableIdsNeedBootstrap = null;
    if (tableIdsNeedBootstrap != null) {
      nonExistingTableIdsNeedBootstrap =
          tableIdsNeedBootstrap.stream()
              .filter(nonExistingTableIds::contains)
              .collect(Collectors.toSet());
    }
    addTables(nonExistingTableIds, nonExistingTableIdsNeedBootstrap);
    if (areIndexTables) {
      this.updateIndexTableForTables(tableIds, true /* indexTable */);
    }
  }

  public void addTablesIfNotExist(Set<String> tableIds, Set<String> tableIdsNeedBootstrap) {
    addTablesIfNotExist(tableIds, tableIdsNeedBootstrap, false /* areIndexTables */);
  }

  public void addTablesIfNotExist(
      Set<String> tableIds, XClusterConfigCreateFormData.BootstrapParams bootstrapParams) {
    addTablesIfNotExist(tableIds, bootstrapParams != null ? bootstrapParams.tables : null);
  }

  @Transactional
  public void addTablesIfNotExist(Set<String> tableIds) {
    addTablesIfNotExist(tableIds, (Set<String>) null /* tableIdsNeedBootstrap */);
  }

  @Transactional
  public void addTables(Set<String> tableIds) {
    addTables(tableIds, null /* tableIdsNeedBootstrap */);
  }

  @Transactional
  public void addTables(Map<String, String> tableIdsStreamIdsMap) {
    tableIdsStreamIdsMap.forEach(
        (tableId, streamId) -> {
          XClusterTableConfig tableConfig = new XClusterTableConfig(this, tableId);
          tableConfig.setStreamId(streamId);
          addTableConfig(tableConfig);
        });
    update();
  }

  @JsonIgnore
  public Set<String> getStreamIdsWithReplicationSetup() {
    return this.getTables().stream()
        .filter(XClusterTableConfig::isReplicationSetupDone)
        .map(XClusterTableConfig::getStreamId)
        .collect(Collectors.toSet());
  }

  @JsonIgnore
  public Map<String, String> getTableIdStreamIdMap(Set<String> tableIds) {
    Set<XClusterTableConfig> tableConfigs = getTablesById(tableIds);
    Map<String, String> tableIdStreamIdMap = new HashMap<>();
    tableConfigs.forEach(
        tableConfig -> tableIdStreamIdMap.put(tableConfig.getTableId(), tableConfig.getStreamId()));
    return tableIdStreamIdMap;
  }

  @Transactional
  public void updateReplicationSetupDone(
      Collection<String> tableIds, boolean replicationSetupDone) {
    // Ensure there is no duplicate in the tableIds collection.
    if (tableIds.size() != new HashSet<>(tableIds).size()) {
      String errMsg = String.format("There are duplicate values in tableIds: %s", tableIds);
      throw new RuntimeException(errMsg);
    }
    for (String tableId : tableIds) {
      Optional<XClusterTableConfig> tableConfig = maybeGetTableById(tableId);
      if (tableConfig.isPresent()) {
        tableConfig.get().setReplicationSetupDone(replicationSetupDone);
      } else {
        String errMsg =
            String.format(
                "Could not find tableId (%s) in the xCluster config with uuid (%s)",
                tableId, getUuid());
        throw new RuntimeException(errMsg);
      }
    }
    log.info(
        "Replication for tables {} in xCluster config {} is set to {}",
        tableIds,
        getName(),
        replicationSetupDone);
    update();
  }

  public void updateReplicationSetupDone(Collection<String> tableIds) {
    updateReplicationSetupDone(tableIds, true /* replicationSetupDone */);
  }

  @Transactional
  public void removeTables(Set<String> tableIds) {
    if (this.getTables() == null) {
      log.debug("No tables is set for xCluster config {}", this.getUuid());
      return;
    }
    for (String tableId : tableIds) {
      if (!this.getTables().removeIf(tableConfig -> tableConfig.getTableId().equals(tableId))) {
        log.debug(
            "Table with id {} was not found to delete in xCluster config {}",
            tableId,
            this.getUuid());
      }
    }
    update();
  }

  @Transactional
  public void updateBackupForTables(Set<String> tableIds, Backup backup) {
    ensureTableIdsExist(tableIds);
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setBackup(backup));
    update();
  }

  @Transactional
  public void updateRestoreForTables(Set<String> tableIds, Restore restore) {
    ensureTableIdsExist(tableIds);
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setRestore(restore));
    update();
  }

  @Transactional
  public void updateRestoreTimeForTables(Set<String> tableIds, Date restoreTime, UUID taskUUID) {
    ensureTableIdsExist(tableIds);
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(
            tableConfig -> {
              tableConfig.setRestoreTime(restoreTime);
              tableConfig.getRestore().update(taskUUID, Restore.State.Completed);
            });
    update();
  }

  @Transactional
  public void updateNeedBootstrapForTables(Collection<String> tableIds, boolean needBootstrap) {
    ensureTableIdsExist(tableIds);
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setNeedBootstrap(needBootstrap));
    update();
  }

  @Transactional
  public void updateIndexTableForTables(Collection<String> tableIds, boolean indexTable) {
    ensureTableIdsExist(tableIds);
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setIndexTable(indexTable));
    update();
  }

  @Transactional
  public void updateBootstrapCreateTimeForTables(Collection<String> tableIds, Date moment) {
    ensureTableIdsExist(new HashSet<>(tableIds));
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setBootstrapCreateTime(moment));
    update();
  }

  @Transactional
  public void updateStatusForTables(
      Collection<String> tableIds, XClusterTableConfig.Status status) {
    ensureTableIdsExist(new HashSet<>(tableIds));
    this.getTableDetails().stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.getTableId()))
        .forEach(tableConfig -> tableConfig.setStatus(status));
    update();
  }

  @JsonIgnore
  public Set<String> getTableIdsInStatus(
      Collection<String> tableIds, XClusterTableConfig.Status status) {
    return getTableIdsInStatus(tableIds, Collections.singleton(status));
  }

  @JsonIgnore
  public Set<String> getTableIdsInStatus(
      Collection<String> tableIds, Collection<XClusterTableConfig.Status> statuses) {
    ensureTableIdsExist(new HashSet<>(tableIds));
    return this.getTableDetails().stream()
        .filter(
            tableConfig ->
                tableIds.contains(tableConfig.getTableId())
                    && statuses.contains(tableConfig.getStatus()))
        .map(tableConfig -> tableConfig.getTableId())
        .collect(Collectors.toSet());
  }

  @JsonIgnore
  public String getNewReplicationGroupName(UUID sourceUniverseUUID, String configName) {
    if (imported) {
      return configName;
    }
    return sourceUniverseUUID + "_" + configName;
  }

  public void setReplicationGroupName(String replicationGroupName) {
    if (imported) {
      this.replicationGroupName = replicationGroupName;
      return;
    }
    setReplicationGroupName(this.getSourceUniverseUUID(), replicationGroupName /* configName */);
  }

  @JsonIgnore
  public void setReplicationGroupName(UUID sourceUniverseUUID, String configName) {
    replicationGroupName = getNewReplicationGroupName(sourceUniverseUUID, configName);
  }

  public void updateStatus(XClusterConfigStatusType status) {
    this.setStatus(status);
    update();
  }

  public void enable() {
    if (!isPaused()) {
      log.info("xCluster config {} is already enabled", this);
    }
    setPaused(false);
    update();
  }

  public void disable() {
    if (isPaused()) {
      log.info("xCluster config {} is already disabled", this);
    }
    setPaused(true);
    update();
  }

  public void updatePaused(boolean paused) {
    if (paused) {
      disable();
    } else {
      enable();
    }
  }

  public void reset() {
    this.setStatus(XClusterConfigStatusType.Initialized);
    this.setPaused(false);
    this.getTables().forEach(tableConfig -> tableConfig.setRestoreTime(null));
    this.update();
  }

  @Transactional
  public static XClusterConfig create(
      String name,
      UUID sourceUniverseUUID,
      UUID targetUniverseUUID,
      XClusterConfigStatusType status,
      boolean imported) {
    XClusterConfig xClusterConfig = new XClusterConfig();
    xClusterConfig.setUuid(UUID.randomUUID());
    xClusterConfig.setName(name);
    xClusterConfig.setSourceUniverseUUID(sourceUniverseUUID);
    xClusterConfig.setTargetUniverseUUID(targetUniverseUUID);
    xClusterConfig.setStatus(status);
    // Imported needs to be set before setReplicationGroupName() call.
    xClusterConfig.setImported(imported);
    xClusterConfig.setPaused(false);
    xClusterConfig.setCreateTime(new Date());
    xClusterConfig.setModifyTime(new Date());
    xClusterConfig.setTableType(TableType.UNKNOWN);
    xClusterConfig.setType(ConfigType.Basic);
    // Set the following variables to their default value. They will be only used for txn
    // xCluster configs.
    xClusterConfig.setSourceActive(
        XClusterConfigTaskBase.TRANSACTION_SOURCE_UNIVERSE_ROLE_ACTIVE_DEFAULT);
    xClusterConfig.setTargetActive(
        XClusterConfigTaskBase.TRANSACTION_TARGET_UNIVERSE_ROLE_ACTIVE_DEFAULT);
    xClusterConfig.setReplicationGroupName(name);
    xClusterConfig.save();
    return xClusterConfig;
  }

  @Transactional
  public static XClusterConfig create(
      String name, UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return create(
        name,
        sourceUniverseUUID,
        targetUniverseUUID,
        XClusterConfigStatusType.Initialized,
        false /* imported */);
  }

  public static XClusterConfig create(
      String name,
      UUID sourceUniverseUUID,
      UUID targetUniverseUUID,
      ConfigType type,
      @Nullable Set<String> tableIds,
      @Nullable Set<String> tableIdsToBootstrap,
      boolean imported) {
    XClusterConfig xClusterConfig =
        create(
            name,
            sourceUniverseUUID,
            targetUniverseUUID,
            XClusterConfigStatusType.Initialized,
            imported);
    // The default type is Basic. If it is set to be txn, then save it in the object.
    if (Objects.equals(type, ConfigType.Txn)) {
      xClusterConfig.setType(ConfigType.Txn);
    }
    if (Objects.nonNull(tableIds) && Objects.nonNull(tableIdsToBootstrap)) {
      xClusterConfig.updateTables(tableIds, tableIdsToBootstrap);
    } else if (Objects.nonNull(tableIds)) {
      xClusterConfig.updateTables(tableIds);
    }
    return xClusterConfig;
  }

  public static XClusterConfig create(
      String name,
      UUID sourceUniverseUUID,
      UUID targetUniverseUUID,
      ConfigType type,
      boolean imported) {
    return create(
        name,
        sourceUniverseUUID,
        targetUniverseUUID,
        type,
        null /* tableIds */,
        null /* tableIdsToBootstrap */,
        imported);
  }

  public static XClusterConfig create(
      String name, UUID sourceUniverseUUID, UUID targetUniverseUUID, ConfigType type) {
    return create(name, sourceUniverseUUID, targetUniverseUUID, type, false /* imported */);
  }

  @Transactional
  public static XClusterConfig create(XClusterConfigCreateFormData createFormData) {
    return createFormData.bootstrapParams == null
        ? create(
            createFormData.name,
            createFormData.sourceUniverseUUID,
            createFormData.targetUniverseUUID,
            createFormData.configType,
            createFormData.tables,
            null /* tableIdsToBootstrap */,
            false /* imported */)
        : create(
            createFormData.name,
            createFormData.sourceUniverseUUID,
            createFormData.targetUniverseUUID,
            createFormData.configType,
            createFormData.tables,
            createFormData.bootstrapParams.tables,
            false /* imported */);
  }

  @Transactional
  public static XClusterConfig create(
      XClusterConfigCreateFormData createFormData,
      List<MasterDdlOuterClass.ListTablesResponsePB.TableInfo> requestedTableInfoList) {
    XClusterConfig xClusterConfig = create(createFormData);
    xClusterConfig.updateTableType(requestedTableInfoList);
    return xClusterConfig;
  }

  @VisibleForTesting
  @Transactional
  public static XClusterConfig create(
      XClusterConfigCreateFormData createFormData, XClusterConfigStatusType status) {
    XClusterConfig xClusterConfig = create(createFormData);
    xClusterConfig.updateStatus(status);
    if (status == XClusterConfigStatusType.Running) {
      xClusterConfig.updateReplicationSetupDone(createFormData.tables);
    }
    return xClusterConfig;
  }

  @Override
  public void update() {
    this.setModifyTime(new Date());
    super.update();
  }

  public static XClusterConfig getValidConfigOrBadRequest(
      Customer customer, UUID xClusterConfigUUID) {
    XClusterConfig xClusterConfig = getOrBadRequest(xClusterConfigUUID);
    checkXClusterConfigInCustomer(xClusterConfig, customer);
    return xClusterConfig;
  }

  public static XClusterConfig getOrBadRequest(UUID xClusterConfigUUID) {
    return maybeGet(xClusterConfigUUID)
        .orElseThrow(
            () ->
                new PlatformServiceException(
                    BAD_REQUEST, "Cannot find XClusterConfig " + xClusterConfigUUID));
  }

  public static Optional<XClusterConfig> maybeGet(UUID xClusterConfigUUID) {
    XClusterConfig xClusterConfig =
        find.query().fetch("tables").where().eq("uuid", xClusterConfigUUID).findOne();
    if (xClusterConfig == null) {
      log.info("Cannot find XClusterConfig {}", xClusterConfigUUID);
      return Optional.empty();
    }
    return Optional.of(xClusterConfig);
  }

  public static List<XClusterConfig> getByTargetUniverseUUID(UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("target_universe_uuid", targetUniverseUUID)
        .findList();
  }

  public static List<XClusterConfig> getBySourceUniverseUUID(UUID sourceUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("source_universe_uuid", sourceUniverseUUID)
        .findList();
  }

  public static List<XClusterConfig> getByUniverseUuid(UUID universeUuid) {
    return Stream.concat(
            getBySourceUniverseUUID(universeUuid).stream(),
            getByTargetUniverseUUID(universeUuid).stream())
        .collect(Collectors.toList());
  }

  public static List<XClusterConfig> getBetweenUniverses(
      UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("source_universe_uuid", sourceUniverseUUID)
        .eq("target_universe_uuid", targetUniverseUUID)
        .findList();
  }

  public static XClusterConfig getByNameSourceTarget(
      String name, UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("config_name", name)
        .eq("source_universe_uuid", sourceUniverseUUID)
        .eq("target_universe_uuid", targetUniverseUUID)
        .findOne();
  }

  public static XClusterConfig getByReplicationGroupNameTarget(
      String replicationGroupName, UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("replication_group_name", replicationGroupName)
        .eq("target_universe_uuid", targetUniverseUUID)
        .findOne();
  }

  private static void checkXClusterConfigInCustomer(
      XClusterConfig xClusterConfig, Customer customer) {
    Set<UUID> customerUniverseUUIDs = customer.getUniverseUUIDs();
    if ((xClusterConfig.getSourceUniverseUUID() != null
            && !customerUniverseUUIDs.contains(xClusterConfig.getSourceUniverseUUID()))
        || (xClusterConfig.getTargetUniverseUUID() != null
            && !customerUniverseUUIDs.contains(xClusterConfig.getTargetUniverseUUID()))) {
      throw new PlatformServiceException(
          BAD_REQUEST,
          String.format(
              "XClusterConfig %s doesn't belong to Customer %s",
              xClusterConfig.getUuid(), customer.getUuid()));
    }
  }

  private void addTableConfig(XClusterTableConfig tableConfig) {
    if (!this.getTables().add(tableConfig)) {
      log.debug(
          "Table with id {} already exists in xCluster config ({})",
          tableConfig.getTableId(),
          this.getUuid());
    }
  }

  public void ensureTableIdsExist(Set<String> tableIds) {
    if (tableIds.isEmpty()) {
      return;
    }
    Set<String> tableIdsInXClusterConfig = getTableIds();
    tableIds.forEach(
        tableId -> {
          if (!tableIdsInXClusterConfig.contains(tableId)) {
            throw new RuntimeException(
                String.format(
                    "Could not find tableId (%s) in the xCluster config with uuid (%s)",
                    tableId, this.getUuid()));
          }
        });
  }

  public void ensureTableIdsExist(Collection<String> tableIds) {
    if (tableIds.isEmpty()) {
      return;
    }
    Set<String> tableIdSet = new HashSet<>(tableIds);
    // Ensure there is no duplicate in the tableIds collection.
    if (tableIds.size() != tableIdSet.size()) {
      String errMsg = String.format("There are duplicate values in tableIds: %s", tableIds);
      throw new RuntimeException(errMsg);
    }
    ensureTableIdsExist(tableIdSet);
  }

  public static <T> Set<T> intersectionOf(Set<T> firstSet, Set<T> secondSet) {
    if (firstSet == null || secondSet == null) {
      return new HashSet<>();
    }
    Set<T> intersection = new HashSet<>(firstSet);
    intersection.retainAll(secondSet);
    return intersection;
  }

  public static boolean isUniverseXClusterParticipant(UUID universeUUID) {
    return !CollectionUtils.isEmpty(getByUniverseUuid(universeUUID));
  }
}
