// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cocos2d.h"
class TrainingScene : public cocos2d::Scene {
public:
    CREATE_FUNC(TrainingScene);
    bool init() override;
    void update(float delta) override;
private:
    cocos2d::Vec2 position{40,240};
    cocos2d::DrawNode *drawing=nullptr;
    bool left=false,right=false,up=false,down=false;
    int score=0;
};
