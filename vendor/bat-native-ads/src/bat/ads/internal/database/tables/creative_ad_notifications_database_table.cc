/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ads/internal/database/tables/creative_ad_notifications_database_table.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "bat/ads/ads_client.h"
#include "bat/ads/internal/ads_client_helper.h"
#include "bat/ads/internal/bundle/creative_ad_info_aliases.h"
#include "bat/ads/internal/bundle/creative_ad_notification_info.h"
#include "bat/ads/internal/container_util.h"
#include "bat/ads/internal/database/database_statement_util.h"
#include "bat/ads/internal/database/database_table_util.h"
#include "bat/ads/internal/database/database_util.h"
#include "bat/ads/internal/database/tables/campaigns_database_table.h"
#include "bat/ads/internal/database/tables/creative_ads_database_table.h"
#include "bat/ads/internal/database/tables/dayparts_database_table.h"
#include "bat/ads/internal/database/tables/geo_targets_database_table.h"
#include "bat/ads/internal/database/tables/segments_database_table.h"
#include "bat/ads/internal/logging.h"
#include "bat/ads/internal/segments/segments_util.h"
#include "bat/ads/internal/time_formatting_util.h"

namespace ads {
namespace database {
namespace table {

namespace {

const char kTableName[] = "creative_ad_notifications";

const int kDefaultBatchSize = 50;

}  // namespace

CreativeAdNotifications::CreativeAdNotifications()
    : batch_size_(kDefaultBatchSize),
      campaigns_database_table_(std::make_unique<Campaigns>()),
      creative_ads_database_table_(std::make_unique<CreativeAds>()),
      dayparts_database_table_(std::make_unique<Dayparts>()),
      geo_targets_database_table_(std::make_unique<GeoTargets>()),
      segments_database_table_(std::make_unique<Segments>()) {}

CreativeAdNotifications::~CreativeAdNotifications() = default;

void CreativeAdNotifications::Save(
    const CreativeAdNotificationList& creative_ad_notifications,
    ResultCallback callback) {
  if (creative_ad_notifications.empty()) {
    callback(/* success */ true);
    return;
  }

  mojom::DBTransactionPtr transaction = mojom::DBTransaction::New();

  const std::vector<CreativeAdNotificationList> batches =
      SplitVector(creative_ad_notifications, batch_size_);

  for (const auto& batch : batches) {
    InsertOrUpdate(transaction.get(), batch);

    CreativeAdList creative_ads(batch.cbegin(), batch.cend());
    campaigns_database_table_->InsertOrUpdate(transaction.get(), creative_ads);
    segments_database_table_->InsertOrUpdate(transaction.get(), creative_ads);
    creative_ads_database_table_->InsertOrUpdate(transaction.get(),
                                                 creative_ads);
    dayparts_database_table_->InsertOrUpdate(transaction.get(), creative_ads);
    geo_targets_database_table_->InsertOrUpdate(transaction.get(),
                                                creative_ads);
  }

  AdsClientHelper::Get()->RunDBTransaction(
      std::move(transaction),
      std::bind(&OnResultCallback, std::placeholders::_1, callback));
}

void CreativeAdNotifications::Delete(ResultCallback callback) {
  mojom::DBTransactionPtr transaction = mojom::DBTransaction::New();

  util::Delete(transaction.get(), GetTableName());

  AdsClientHelper::Get()->RunDBTransaction(
      std::move(transaction),
      std::bind(&OnResultCallback, std::placeholders::_1, callback));
}

void CreativeAdNotifications::GetForSegments(
    const SegmentList& segments,
    GetCreativeAdNotificationsCallback callback) {
  if (segments.empty()) {
    callback(/* success */ true, segments, {});
    return;
  }

  const std::string query = base::StringPrintf(
      "SELECT "
      "can.creative_instance_id, "
      "can.creative_set_id, "
      "can.campaign_id, "
      "cam.start_at_timestamp, "
      "cam.end_at_timestamp, "
      "cam.daily_cap, "
      "cam.advertiser_id, "
      "cam.priority, "
      "ca.conversion, "
      "ca.per_day, "
      "ca.per_week, "
      "ca.per_month, "
      "ca.total_max, "
      "ca.value, "
      "ca.split_test_group, "
      "s.segment, "
      "gt.geo_target, "
      "ca.target_url, "
      "can.title, "
      "can.body, "
      "cam.ptr, "
      "dp.dow, "
      "dp.start_minute, "
      "dp.end_minute "
      "FROM %s AS can "
      "INNER JOIN campaigns AS cam "
      "ON cam.campaign_id = can.campaign_id "
      "INNER JOIN segments AS s "
      "ON s.creative_set_id = can.creative_set_id "
      "INNER JOIN creative_ads AS ca "
      "ON ca.creative_instance_id = can.creative_instance_id "
      "INNER JOIN geo_targets AS gt "
      "ON gt.campaign_id = can.campaign_id "
      "INNER JOIN dayparts AS dp "
      "ON dp.campaign_id = can.campaign_id "
      "WHERE s.segment IN %s "
      "AND %s BETWEEN cam.start_at_timestamp AND cam.end_at_timestamp",
      GetTableName().c_str(),
      BuildBindingParameterPlaceholder(segments.size()).c_str(),
      TimeAsTimestampString(base::Time::Now()).c_str());

  mojom::DBCommandPtr command = mojom::DBCommand::New();
  command->type = mojom::DBCommand::Type::READ;
  command->command = query;

  int index = 0;
  for (const auto& segment : segments) {
    BindString(command.get(), index, base::ToLowerASCII(segment));
    index++;
  }

  command->record_bindings = {
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // creative_instance_id
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // creative_set_id
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // campaign_id
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // start_at
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // end_at
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // daily_cap
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // advertiser_id
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // priority
      mojom::DBCommand::RecordBindingType::BOOL_TYPE,    // conversion
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_day
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_week
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_month
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // total_max
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // value
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // split_test_group
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // segment
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // geo_target
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // target_url
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // title
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // body
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // ptr
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // dayparts->dow
      mojom::DBCommand::RecordBindingType::INT_TYPE,  // dayparts->start_minute
      mojom::DBCommand::RecordBindingType::INT_TYPE   // dayparts->end_minute
  };

  mojom::DBTransactionPtr transaction = mojom::DBTransaction::New();
  transaction->commands.push_back(std::move(command));

  AdsClientHelper::Get()->RunDBTransaction(
      std::move(transaction),
      std::bind(&CreativeAdNotifications::OnGetForSegments, this,
                std::placeholders::_1, segments, callback));
}

void CreativeAdNotifications::GetAll(
    GetCreativeAdNotificationsCallback callback) {
  const std::string query = base::StringPrintf(
      "SELECT "
      "can.creative_instance_id, "
      "can.creative_set_id, "
      "can.campaign_id, "
      "cam.start_at_timestamp, "
      "cam.end_at_timestamp, "
      "cam.daily_cap, "
      "cam.advertiser_id, "
      "cam.priority, "
      "ca.conversion, "
      "ca.per_day, "
      "ca.per_week, "
      "ca.per_month, "
      "ca.total_max, "
      "ca.value, "
      "ca.split_test_group, "
      "s.segment, "
      "gt.geo_target, "
      "ca.target_url, "
      "can.title, "
      "can.body, "
      "cam.ptr, "
      "dp.dow, "
      "dp.start_minute, "
      "dp.end_minute "
      "FROM %s AS can "
      "INNER JOIN campaigns AS cam "
      "ON cam.campaign_id = can.campaign_id "
      "INNER JOIN segments AS s "
      "ON s.creative_set_id = can.creative_set_id "
      "INNER JOIN creative_ads AS ca "
      "ON ca.creative_instance_id = can.creative_instance_id "
      "INNER JOIN geo_targets AS gt "
      "ON gt.campaign_id = can.campaign_id "
      "INNER JOIN dayparts AS dp "
      "ON dp.campaign_id = can.campaign_id "
      "WHERE %s BETWEEN cam.start_at_timestamp AND cam.end_at_timestamp",
      GetTableName().c_str(), TimeAsTimestampString(base::Time::Now()).c_str());

  mojom::DBCommandPtr command = mojom::DBCommand::New();
  command->type = mojom::DBCommand::Type::READ;
  command->command = query;

  command->record_bindings = {
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // creative_instance_id
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // creative_set_id
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // campaign_id
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // start_at
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // end_at
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // daily_cap
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // advertiser_id
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // priority
      mojom::DBCommand::RecordBindingType::BOOL_TYPE,    // conversion
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_day
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_week
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // per_month
      mojom::DBCommand::RecordBindingType::INT_TYPE,     // total_max
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // value
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // split_test_group
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // segment
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // geo_target
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // target_url
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // title
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // body
      mojom::DBCommand::RecordBindingType::DOUBLE_TYPE,  // ptr
      mojom::DBCommand::RecordBindingType::STRING_TYPE,  // dayparts->dow
      mojom::DBCommand::RecordBindingType::INT_TYPE,  // dayparts->start_minute
      mojom::DBCommand::RecordBindingType::INT_TYPE   // dayparts->end_minute
  };

  mojom::DBTransactionPtr transaction = mojom::DBTransaction::New();
  transaction->commands.push_back(std::move(command));

  AdsClientHelper::Get()->RunDBTransaction(
      std::move(transaction), std::bind(&CreativeAdNotifications::OnGetAll,
                                        this, std::placeholders::_1, callback));
}

std::string CreativeAdNotifications::GetTableName() const {
  return kTableName;
}

void CreativeAdNotifications::Migrate(mojom::DBTransaction* transaction,
                                      const int to_version) {
  DCHECK(transaction);

  switch (to_version) {
    case 16: {
      MigrateToV16(transaction);
      break;
    }

    default: {
      break;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

void CreativeAdNotifications::InsertOrUpdate(
    mojom::DBTransaction* transaction,
    const CreativeAdNotificationList& creative_ad_notifications) {
  DCHECK(transaction);

  if (creative_ad_notifications.empty()) {
    return;
  }

  mojom::DBCommandPtr command = mojom::DBCommand::New();
  command->type = mojom::DBCommand::Type::RUN;
  command->command =
      BuildInsertOrUpdateQuery(command.get(), creative_ad_notifications);

  transaction->commands.push_back(std::move(command));
}

int CreativeAdNotifications::BindParameters(
    mojom::DBCommand* command,
    const CreativeAdNotificationList& creative_ad_notifications) {
  DCHECK(command);

  int count = 0;

  int index = 0;
  for (const auto& creative_ad_notification : creative_ad_notifications) {
    BindString(command, index++, creative_ad_notification.creative_instance_id);
    BindString(command, index++, creative_ad_notification.creative_set_id);
    BindString(command, index++, creative_ad_notification.campaign_id);
    BindString(command, index++, creative_ad_notification.title);
    BindString(command, index++, creative_ad_notification.body);

    count++;
  }

  return count;
}

std::string CreativeAdNotifications::BuildInsertOrUpdateQuery(
    mojom::DBCommand* command,
    const CreativeAdNotificationList& creative_ad_notifications) {
  const int count = BindParameters(command, creative_ad_notifications);

  return base::StringPrintf(
      "INSERT OR REPLACE INTO %s "
      "(creative_instance_id, "
      "creative_set_id, "
      "campaign_id, "
      "title, "
      "body) VALUES %s",
      GetTableName().c_str(),
      BuildBindingParameterPlaceholders(5, count).c_str());
}

void CreativeAdNotifications::OnGetForSegments(
    mojom::DBCommandResponsePtr response,
    const SegmentList& segments,
    GetCreativeAdNotificationsCallback callback) {
  if (!response ||
      response->status != mojom::DBCommandResponse::Status::RESPONSE_OK) {
    BLOG(0, "Failed to get creative ad notifications");
    callback(/* success */ false, segments, {});
    return;
  }

  CreativeAdNotificationList creative_ads =
      GetCreativeAdsFromResponse(std::move(response));

  callback(/* success */ true, segments, creative_ads);
}

void CreativeAdNotifications::OnGetAll(
    mojom::DBCommandResponsePtr response,
    GetCreativeAdNotificationsCallback callback) {
  if (!response ||
      response->status != mojom::DBCommandResponse::Status::RESPONSE_OK) {
    BLOG(0, "Failed to get all creative ad notifications");
    callback(/* success */ false, {}, {});
    return;
  }

  const CreativeAdNotificationList creative_ads =
      GetCreativeAdsFromResponse(std::move(response));

  const SegmentList segments = GetSegments(creative_ads);

  callback(/* success */ true, segments, creative_ads);
}

CreativeAdNotificationInfo CreativeAdNotifications::GetFromRecord(
    mojom::DBRecord* record) const {
  CreativeAdNotificationInfo creative_ad_notification;

  creative_ad_notification.creative_instance_id = ColumnString(record, 0);
  creative_ad_notification.creative_set_id = ColumnString(record, 1);
  creative_ad_notification.campaign_id = ColumnString(record, 2);
  creative_ad_notification.start_at =
      base::Time::FromDoubleT(ColumnDouble(record, 3));
  creative_ad_notification.end_at =
      base::Time::FromDoubleT(ColumnDouble(record, 4));
  creative_ad_notification.daily_cap = ColumnInt(record, 5);
  creative_ad_notification.advertiser_id = ColumnString(record, 6);
  creative_ad_notification.priority = ColumnInt(record, 7);
  creative_ad_notification.conversion = ColumnBool(record, 8);
  creative_ad_notification.per_day = ColumnInt(record, 9);
  creative_ad_notification.per_week = ColumnInt(record, 10);
  creative_ad_notification.per_month = ColumnInt(record, 11);
  creative_ad_notification.total_max = ColumnInt(record, 12);
  creative_ad_notification.value = ColumnDouble(record, 13);
  creative_ad_notification.split_test_group = ColumnString(record, 14);
  creative_ad_notification.segment = ColumnString(record, 15);
  creative_ad_notification.geo_targets.insert(ColumnString(record, 16));
  creative_ad_notification.target_url = ColumnString(record, 17);
  creative_ad_notification.title = ColumnString(record, 18);
  creative_ad_notification.body = ColumnString(record, 19);
  creative_ad_notification.ptr = ColumnDouble(record, 20);

  CreativeDaypartInfo daypart;
  daypart.dow = ColumnString(record, 21);
  daypart.start_minute = ColumnInt(record, 22);
  daypart.end_minute = ColumnInt(record, 23);
  creative_ad_notification.dayparts.push_back(daypart);

  return creative_ad_notification;
}

CreativeAdNotificationMap CreativeAdNotifications::GroupCreativeAdsFromResponse(
    mojom::DBCommandResponsePtr response) {
  DCHECK(response);

  CreativeAdNotificationMap creative_ads;

  for (const auto& record : response->result->get_records()) {
    const CreativeAdNotificationInfo creative_ad = GetFromRecord(record.get());

    const auto iter = creative_ads.find(creative_ad.creative_instance_id);
    if (iter == creative_ads.end()) {
      creative_ads.insert({creative_ad.creative_instance_id, creative_ad});
      continue;
    }

    // Creative instance already exists, so append the geo targets and dayparts
    // to the existing creative ad
    iter->second.geo_targets.insert(creative_ad.geo_targets.begin(),
                                    creative_ad.geo_targets.end());

    iter->second.dayparts.insert(iter->second.dayparts.end(),
                                 creative_ad.dayparts.begin(),
                                 creative_ad.dayparts.end());
  }

  return creative_ads;
}

CreativeAdNotificationList CreativeAdNotifications::GetCreativeAdsFromResponse(
    mojom::DBCommandResponsePtr response) {
  DCHECK(response);

  const CreativeAdNotificationMap grouped_creative_ads =
      GroupCreativeAdsFromResponse(std::move(response));

  CreativeAdNotificationList creative_ads;
  for (const auto& grouped_creative_ad : grouped_creative_ads) {
    const CreativeAdNotificationInfo creative_ad = grouped_creative_ad.second;
    creative_ads.push_back(creative_ad);
  }

  return creative_ads;
}

void CreativeAdNotifications::CreateTableV16(
    mojom::DBTransaction* transaction) {
  DCHECK(transaction);

  const std::string query = base::StringPrintf(
      "CREATE TABLE %s "
      "(creative_instance_id TEXT NOT NULL PRIMARY KEY UNIQUE "
      "ON CONFLICT REPLACE, "
      "creative_set_id TEXT NOT NULL, "
      "campaign_id TEXT NOT NULL, "
      "title TEXT NOT NULL, "
      "body TEXT NOT NULL)",
      GetTableName().c_str());

  mojom::DBCommandPtr command = mojom::DBCommand::New();
  command->type = mojom::DBCommand::Type::EXECUTE;
  command->command = query;

  transaction->commands.push_back(std::move(command));
}

void CreativeAdNotifications::MigrateToV16(mojom::DBTransaction* transaction) {
  DCHECK(transaction);

  util::Drop(transaction, GetTableName());

  CreateTableV16(transaction);
}

}  // namespace table
}  // namespace database
}  // namespace ads
